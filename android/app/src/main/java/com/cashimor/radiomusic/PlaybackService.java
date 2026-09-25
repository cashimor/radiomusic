package com.cashimor.radiomusic;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.MediaPlayer;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.IBinder;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class PlaybackService extends Service {
    public static final String ACTION_PLAY = "com.cashimor.radiomusic.PLAY";
    public static final String ACTION_PAUSE = "com.cashimor.radiomusic.PAUSE";
    public static final String ACTION_STOP = "com.cashimor.radiomusic.STOP";
    public static final String ACTION_QUERY = "com.cashimor.radiomusic.QUERY";
    public static final String ACTION_STATE = "com.cashimor.radiomusic.STATE";
    public static final String ACTION_RADIO_START = "com.cashimor.radiomusic.RADIO_START";
    public static final String ACTION_RADIO_STOP = "com.cashimor.radiomusic.RADIO_STOP";
    public static final String ACTION_HEAR_RADIO = "com.cashimor.radiomusic.HEAR_RADIO";
    public static final String ACTION_RETURN_REMIX = "com.cashimor.radiomusic.RETURN_REMIX";
    public static final String EXTRA_BUSY = "busy";
    public static final String EXTRA_PLAYING = "playing";
    public static final String EXTRA_PAUSED = "paused";
    public static final String EXTRA_MESSAGE = "message";
    public static final String EXTRA_RADIO = "radio";
    public static final String EXTRA_HEARING_RADIO = "hearing_radio";

    private static final String CHANNEL_ID = "radiomusic_playback";
    private static final int NOTIFICATION_ID = 42;
    static { System.loadLibrary("RadioNative"); }
    private static native long nativeCreate(String directory);
    private static native String nativeStart(long player);
    private static native void nativePause(long player);
    private static native void nativeResume(long player);
    private static native void nativeNextSequence(long player);
    private static native void nativeRepeatCurrent(long player);
    private static native void nativeStop(long player);
    private static native void nativeDestroy(long player);
    private static native void nativeFeedMp3(long player, byte[] data);
    private static native void nativeHearRadio(long player, boolean enabled);
    private static native void nativeResetRadioInput(long player);

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private long player;
    private boolean loading, playing, paused, receiverRegistered, focusHeld, resumeOnFocusGain;
    private boolean radioCapturing, hearingRadio;
    private final Object nativeLock = new Object();
    private RadioStream radioStream;
    private String message = "Ready to play your saved loops.";
    private MediaSession mediaSession;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;

    private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) { pausePlayback("Paused because the audio output was disconnected."); }
    };

    private final AudioManager.OnAudioFocusChangeListener focusListener = change -> {
        if (change == AudioManager.AUDIOFOCUS_GAIN) {
            focusHeld = true;
            if (resumeOnFocusGain) { resumeOnFocusGain = false; resumePlayback(); }
        } else if (change == AudioManager.AUDIOFOCUS_LOSS || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT
                || change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK) {
            resumeOnFocusGain = change != AudioManager.AUDIOFOCUS_LOSS && playing && !paused;
            if (change == AudioManager.AUDIOFOCUS_LOSS) abandonAudioFocus();
            if (playing && !paused) pausePlayback(change == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK
                    ? "Paused while another app uses audio." : "Paused for another audio app.");
        }
    };

    @Override public void onCreate() {
        super.onCreate();
        audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (Build.VERSION.SDK_INT >= 26) {
            AudioAttributes attributes = new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build();
            focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attributes).setOnAudioFocusChangeListener(focusListener)
                    .setWillPauseWhenDucked(true).build();
        }
        createMediaSession();
        createNotificationChannel();
        IntentFilter filter = new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(noisyReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(noisyReceiver, filter);
        receiverRegistered = true;
    }

    private void createMediaSession() {
        mediaSession = new MediaSession(this, "RadioMusic");
        mediaSession.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { play(); }
            @Override public void onPause() { pausePlayback("Paused."); }
            @Override public void onStop() { stopPlayback(); }
            @Override public void onSkipToNext() { skipToNextSequence(); }
            @Override public void onSkipToPrevious() { repeatCurrentSequence(); }
        });
        mediaSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        mediaSession.setMetadata(new MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE, "RadioMusic")
                .putString(MediaMetadata.METADATA_KEY_ARTIST, "Evolving loop mix").build());
        mediaSession.setActive(true);
        updateSession();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_QUERY : intent.getAction();
        if (ACTION_PLAY.equals(action)) { ensureForeground(); play(); }
        else if (ACTION_PAUSE.equals(action)) pausePlayback("Paused.");
        else if (ACTION_STOP.equals(action)) stopPlayback();
        else if (ACTION_RADIO_START.equals(action)) { ensureForeground(); startRadioCapture(); }
        else if (ACTION_RADIO_STOP.equals(action)) stopRadioCapture();
        else if (ACTION_HEAR_RADIO.equals(action)) setHearRadio(true);
        else if (ACTION_RETURN_REMIX.equals(action)) setHearRadio(false);
        else publishState();
        return START_NOT_STICKY;
    }

    private void ensureForeground() {
        startForeground(NOTIFICATION_ID, buildNotification());
    }

    private void play() {
        if (loading || (playing && !paused)) return;
        if (!requestAudioFocus()) { message = "Another app is using audio. Try again shortly."; publishState(); return; }
        if (player != 0) {
            nativeResume(player); playing = true; paused = false; message = "Remix playing."; updateSession(); publishState(); return;
        }
        loading = true; message = "Loading loops and starting audio…"; updateSession(); publishState();
        ensureForeground();
        worker.execute(() -> {
            long created;
            String error;
            synchronized (nativeLock) {
                created = nativeCreate(new File("/storage/emulated/0/Documents/Radiomusic/loops").getAbsolutePath());
                error = nativeStart(created);
            }
            main.post(() -> {
                loading = false;
                if (error == null || error.isEmpty()) {
                    player = created; playing = true; paused = false;
                    message = "Remix playing. It will evolve gradually as the phrase changes.";
                } else {
                    synchronized (nativeLock) { nativeDestroy(created); }
                    playing = false; paused = false; message = error; abandonAudioFocus();
                }
                updateSession(); publishState();
            });
        });
    }

    private boolean requestAudioFocus() {
        int result = Build.VERSION.SDK_INT >= 26 ? audioManager.requestAudioFocus(focusRequest)
                : audioManager.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
        focusHeld = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        return focusHeld;
    }

    private void abandonAudioFocus() {
        if (audioManager == null || !focusHeld) return;
        if (Build.VERSION.SDK_INT >= 26) audioManager.abandonAudioFocusRequest(focusRequest);
        else audioManager.abandonAudioFocus(focusListener);
        focusHeld = false;
    }

    private void pausePlayback(String text) {
        if (player == 0 || !playing || paused) { message = text; publishState(); return; }
        synchronized (nativeLock) { nativePause(player); }
        paused = true; message = text; updateSession(); publishState();
    }

    private void resumePlayback() {
        if (player == 0 || !playing || !paused) return;
        synchronized (nativeLock) { nativeResume(player); }
        paused = false; message = "Remix playing."; updateSession(); publishState();
    }

    private void skipToNextSequence() {
        if (player == 0 || !playing || paused) return;
        synchronized (nativeLock) { nativeNextSequence(player); }
    }

    private void repeatCurrentSequence() {
        if (player == 0 || !playing || paused) return;
        synchronized (nativeLock) { nativeRepeatCurrent(player); }
    }

    private void startRadioCapture() {
        if (radioCapturing || loading) return;
        if (!requestAudioFocus()) { message = "Another app is using audio. Try again shortly."; publishState(); return; }
        if (player != 0) { beginRadioStream(); return; }
        loading = true; message = "Preparing the remix and Wi-Fi radio capture…"; ensureForeground(); publishState();
        worker.execute(() -> {
            long created; String error;
            synchronized (nativeLock) {
                created = nativeCreate(new File("/storage/emulated/0/Documents/Radiomusic/loops").getAbsolutePath());
                error = nativeStart(created);
            }
            main.post(() -> {
                loading = false;
                if (error == null || error.isEmpty()) { player = created; playing = true; paused = false; beginRadioStream(); }
                else { synchronized (nativeLock) { nativeDestroy(created); } message = error; abandonAudioFocus(); publishState(); }
            });
        });
    }

    private void beginRadioStream() {
        if (radioStream == null) radioStream = new RadioStream(this,
                text -> main.post(() -> { message = text; publishState(); }), new RadioStream.Feed() {
                    @Override public void onConnected() { resetRadioInput(); }
                    @Override public void onBytes(byte[] bytes) { feedRadioBytes(bytes); }
                });
        if (paused && player != 0) resumePlayback();
        radioCapturing = radioStream.start();
        hearingRadio = radioCapturing;
        synchronized (nativeLock) { if (player != 0) nativeHearRadio(player, hearingRadio); }
        updateSession(); publishState();
    }

    private void feedRadioBytes(byte[] bytes) {
        synchronized (nativeLock) { if (player != 0 && radioCapturing) nativeFeedMp3(player, bytes); }
    }

    private void resetRadioInput() {
        synchronized (nativeLock) { if (player != 0 && radioCapturing) nativeResetRadioInput(player); }
    }

    private void stopRadioCapture() {
        if (radioStream != null) radioStream.stop();
        radioCapturing = false;
        if (player != 0) synchronized (nativeLock) { nativeHearRadio(player, false); }
        hearingRadio = false; message = "Radio capture stopped. The remix can continue playing."; publishState();
    }

    private void setHearRadio(boolean enabled) {
        if (!radioCapturing || player == 0) return;
        synchronized (nativeLock) { nativeHearRadio(player, enabled); }
        hearingRadio = enabled; message = enabled ? "Listening to the live station over Wi-Fi." : "Returning gradually to the remix.";
        publishState();
    }

    private void stopPlayback() {
        if (radioStream != null) radioStream.stop();
        radioCapturing = false; hearingRadio = false;
        synchronized (nativeLock) { if (player != 0) { nativeStop(player); nativeDestroy(player); player = 0; } }
        playing = false; paused = false; loading = false; message = "Playback stopped.";
        abandonAudioFocus(); updateSession(); publishState();
        stopForeground(true);
        stopSelf();
    }

    private void updateSession() {
        if (mediaSession == null) return;
        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
                | PlaybackState.ACTION_STOP | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS;
        int state = loading ? PlaybackState.STATE_BUFFERING : !playing ? PlaybackState.STATE_STOPPED
                : paused ? PlaybackState.STATE_PAUSED : PlaybackState.STATE_PLAYING;
        mediaSession.setPlaybackState(new PlaybackState.Builder().setActions(actions).setState(state, 0, 1.0f).build());
        if (mediaSession.isActive() != (playing || paused || loading)) mediaSession.setActive(playing || paused || loading);
        if (playing || paused || loading) getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, buildNotification());
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "Playback", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("RadioMusic playback controls");
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
    }

    private Notification buildNotification() {
        PendingIntent pause = serviceIntent(paused ? ACTION_PLAY : ACTION_PAUSE, 1);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL_ID) : new Notification.Builder(this);
        builder.setSmallIcon(R.drawable.ic_launcher_foreground)
                .setContentTitle("RadioMusic")
                .setContentText(loading ? "Loading your saved loops" : paused ? "Paused" : "Playing your evolving mix")
                .setContentIntent(PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), pendingFlags()))
                .setOngoing(playing || loading)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .addAction(new Notification.Action.Builder(paused ? android.R.drawable.ic_media_play : android.R.drawable.ic_media_pause,
                        paused ? "Play" : "Pause", pause).build())
                .addAction(new Notification.Action.Builder(android.R.drawable.ic_menu_close_clear_cancel, "Stop", serviceIntent(ACTION_STOP, 2)).build());
        if (Build.VERSION.SDK_INT >= 21) builder.setStyle(new Notification.MediaStyle().setMediaSession(mediaSession.getSessionToken()).setShowActionsInCompactView(0, 1));
        return builder.build();
    }

    private PendingIntent serviceIntent(String action, int requestCode) {
        return PendingIntent.getService(this, requestCode, new Intent(this, PlaybackService.class).setAction(action), pendingFlags());
    }
    private int pendingFlags() { return PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0); }

    private void publishState() {
        Intent state = new Intent(ACTION_STATE).setPackage(getPackageName())
                .putExtra(EXTRA_BUSY, loading).putExtra(EXTRA_PLAYING, playing).putExtra(EXTRA_PAUSED, paused).putExtra(EXTRA_MESSAGE, message);
        state.putExtra(EXTRA_RADIO, radioCapturing);
        state.putExtra(EXTRA_HEARING_RADIO, hearingRadio);
        sendBroadcast(state);
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onDestroy() {
        if (radioStream != null) radioStream.stop();
        if (receiverRegistered) unregisterReceiver(noisyReceiver);
        abandonAudioFocus();
        synchronized (nativeLock) { if (player != 0) { nativeStop(player); nativeDestroy(player); player = 0; } }
        if (mediaSession != null) { mediaSession.setActive(false); mediaSession.release(); }
        worker.shutdownNow(); super.onDestroy();
    }
}
