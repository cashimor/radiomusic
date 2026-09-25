package com.cashimor.radiomusic;

import android.Manifest;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    private static final int REQUEST_STORAGE = 1;
    private TextView status;
    private Button playButton, radioButton, monitorButton;
    private boolean playing, paused, busy, radioCapturing, hearingRadio, receiverRegistered;
    private boolean pendingRadioStart;

    private final BroadcastReceiver stateReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            busy = intent.getBooleanExtra(PlaybackService.EXTRA_BUSY, false);
            playing = intent.getBooleanExtra(PlaybackService.EXTRA_PLAYING, false);
            paused = intent.getBooleanExtra(PlaybackService.EXTRA_PAUSED, false);
            radioCapturing = intent.getBooleanExtra(PlaybackService.EXTRA_RADIO, false);
            hearingRadio = intent.getBooleanExtra(PlaybackService.EXTRA_HEARING_RADIO, false);
            String message = intent.getStringExtra(PlaybackService.EXTRA_MESSAGE);
            if (message != null && !message.isEmpty()) status.setText(message);
            else status.setText(playing ? (paused ? "Paused." : "Remix playing. It will evolve gradually as the phrase changes.") : "Ready to play your saved loops.");
            updateButton();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        getWindow().setStatusBarColor(Color.rgb(246, 247, 249));
        getWindow().setNavigationBarColor(Color.rgb(246, 247, 249));
        buildUi();
        refreshState();
    }

    private void buildUi() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        page.setPadding(dp(24), dp(34), dp(24), dp(24));
        page.setBackgroundColor(Color.rgb(246, 247, 249));

        TextView title = new TextView(this);
        title.setText("RADIOMUSIC"); title.setTextSize(27); title.setTextColor(Color.rgb(25, 35, 48));
        title.setGravity(Gravity.CENTER); title.setTypeface(null, 1);
        page.addView(title, matchWrap());

        TextView subtitle = new TextView(this);
        subtitle.setText("A gradual, evolving mix from your saved loops"); subtitle.setTextSize(15);
        subtitle.setTextColor(Color.rgb(97, 112, 130)); subtitle.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subtitleParams = matchWrap(); subtitleParams.topMargin = dp(8);
        page.addView(subtitle, subtitleParams);

        TextView libraryInfo = new TextView(this);
        libraryInfo.setText("Loop library\nDocuments/Radiomusic/loops"); libraryInfo.setTextSize(16);
        libraryInfo.setTextColor(Color.rgb(38, 49, 64)); libraryInfo.setGravity(Gravity.CENTER);
        libraryInfo.setPadding(dp(14), dp(28), dp(14), dp(28));
        LinearLayout.LayoutParams infoParams = matchWrap(); infoParams.topMargin = dp(30);
        page.addView(libraryInfo, infoParams);

        playButton = button("Play remix");
        playButton.setOnClickListener(v -> togglePlayback());
        LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(-1, dp(58));
        buttonParams.topMargin = dp(10); page.addView(playButton, buttonParams);

        radioButton = button("Start radio capture");
        radioButton.setOnClickListener(v -> sendRadioCommand(radioCapturing ? PlaybackService.ACTION_RADIO_STOP : PlaybackService.ACTION_RADIO_START));
        LinearLayout.LayoutParams radioParams = new LinearLayout.LayoutParams(-1, dp(54));
        radioParams.topMargin = dp(10); page.addView(radioButton, radioParams);

        monitorButton = button("Hear live radio");
        monitorButton.setOnClickListener(v -> sendRadioCommand(hearingRadio ? PlaybackService.ACTION_RETURN_REMIX : PlaybackService.ACTION_HEAR_RADIO));
        LinearLayout.LayoutParams monitorParams = new LinearLayout.LayoutParams(-1, dp(54));
        monitorParams.topMargin = dp(8); page.addView(monitorButton, monitorParams);

        status = new TextView(this); status.setTextSize(14); status.setTextColor(Color.rgb(79, 96, 115));
        status.setGravity(Gravity.CENTER); status.setPadding(dp(12), dp(22), dp(12), dp(12));
        page.addView(status, matchWrap());

        TextView note = new TextView(this);
        note.setText("Radio capture uses Wi-Fi only. New recordings are saved automatically and join the remix as it evolves. Playback continues in the background.");
        note.setTextSize(13); note.setTextColor(Color.rgb(111, 124, 140)); note.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams noteParams = matchWrap(); noteParams.topMargin = dp(24);
        page.addView(note, noteParams);
        setContentView(page);
    }

    private Button button(String label) { Button b = new Button(this); b.setText(label); b.setAllCaps(false); b.setTextSize(16); return b; }
    private LinearLayout.LayoutParams matchWrap() { return new LinearLayout.LayoutParams(-1, -2); }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    @Override protected void onStart() {
        super.onStart();
        IntentFilter filter = new IntentFilter(PlaybackService.ACTION_STATE);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(stateReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(stateReceiver, filter);
        receiverRegistered = true;
        refreshState();
    }

    @Override protected void onStop() {
        if (receiverRegistered) { unregisterReceiver(stateReceiver); receiverRegistered = false; }
        super.onStop();
    }

    private void refreshState() {
        Intent query = new Intent(this, PlaybackService.class).setAction(PlaybackService.ACTION_QUERY);
        startService(query);
    }

    private void togglePlayback() {
        if (busy) return;
        if (!hasLibraryAccess()) {
            requestLibraryAccess();
            return;
        }
        String action = playing && !paused ? PlaybackService.ACTION_PAUSE : PlaybackService.ACTION_PLAY;
        Intent command = new Intent(this, PlaybackService.class).setAction(action);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(command); else startService(command);
        busy = action.equals(PlaybackService.ACTION_PLAY) && !playing;
        status.setText(busy ? "Loading loops and starting audio…" : "Updating playback…");
        updateButton();
    }

    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == REQUEST_STORAGE && hasLibraryAccess()) {
            if (pendingRadioStart) { pendingRadioStart = false; sendRadioCommand(PlaybackService.ACTION_RADIO_START); }
            else togglePlayback();
        }
        else if (request == REQUEST_STORAGE) status.setText("Allow music file access to read Documents/Radiomusic/loops.");
    }

    private void updateButton() {
        if (playButton != null) { playButton.setEnabled(!busy); playButton.setText(playing ? (paused ? "Resume remix" : "Pause remix") : "Play remix"); }
        if (radioButton != null) { radioButton.setEnabled(!busy); radioButton.setText(radioCapturing ? "Stop radio capture" : "Start radio capture"); }
        if (monitorButton != null) { monitorButton.setEnabled(radioCapturing); monitorButton.setText(hearingRadio ? "Return to remix" : "Hear live radio"); }
    }

    private boolean hasLibraryAccess() {
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestLibraryAccess() {
        requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_STORAGE);
    }

    private void sendRadioCommand(String action) {
        if (PlaybackService.ACTION_RADIO_START.equals(action) && !hasLibraryAccess()) {
            pendingRadioStart = true;
            requestLibraryAccess();
            return;
        }
        Intent command = new Intent(this, PlaybackService.class).setAction(action);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(command); else startService(command);
        if (PlaybackService.ACTION_RADIO_START.equals(action)) { busy = true; status.setText("Preparing Wi-Fi radio capture…"); updateButton(); }
    }
}
