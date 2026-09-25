package com.cashimor.radiomusic;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.util.Log;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.atomic.AtomicBoolean;

final class RadioStream {
    interface Listener { void onStatus(String text); }
    interface Feed { void onConnected(); void onBytes(byte[] bytes); }
    private static final String STATION_URL = "https://0nlineradio.radioho.st/technolovers-trance";
    private final Context context;
    private final Listener listener;
    private final Feed feed;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private ConnectivityManager connectivity;
    private ConnectivityManager.NetworkCallback callback;
    private volatile Network network;
    private volatile HttpURLConnection connection;
    private Thread streamThread;

    RadioStream(Context context, Listener listener, Feed feed) {
        this.context = context.getApplicationContext(); this.listener = listener; this.feed = feed;
    }

    synchronized boolean start() {
        if (running.get()) return true;
        running.set(true);
        listener.onStatus("Waiting for Wi-Fi to connect to Technolovers Trance…");
        connectivity = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        NetworkRequest request = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onCapabilitiesChanged(Network available, NetworkCapabilities caps) {
                if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                        && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                        && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)) connectOn(available);
            }
            @Override public void onLost(Network lost) {
                if (lost.equals(network)) {
                    network = null;
                    HttpURLConnection active = connection;
                    if (active != null) active.disconnect();
                    listener.onStatus("Wi-Fi disconnected. Radio capture is waiting for Wi-Fi.");
                }
            }
        };
        try { connectivity.requestNetwork(request, callback); }
        catch (RuntimeException error) {
            running.set(false); listener.onStatus("Could not request Wi-Fi: " + error.getMessage());
            return false;
        }
        return true;
    }

    private synchronized void connectOn(Network available) {
        if (!running.get() || available.equals(network)) return;
        network = available;
        if (streamThread != null) {
            HttpURLConnection active = connection;
            if (active != null) active.disconnect();
            streamThread.interrupt();
        }
        streamThread = new Thread(() -> streamLoop(available), "RadioMusic-WiFi-Radio");
        streamThread.start();
    }

    private void streamLoop(Network selectedNetwork) {
        while (running.get() && selectedNetwork.equals(network)) {
            HttpURLConnection active = null;
            try {
                listener.onStatus("Connecting to Technolovers Trance over Wi-Fi…");
                active = (HttpURLConnection) selectedNetwork.openConnection(new URL(STATION_URL));
                connection = active;
                active.setConnectTimeout(12000); active.setReadTimeout(12000);
                active.setRequestProperty("Icy-MetaData", "0");
                active.setRequestProperty("User-Agent", "Radiomusic-Android/0.1");
                active.setInstanceFollowRedirects(true);
                int code = active.getResponseCode();
                String type = active.getContentType();
                Log.i("RadioMusicCapture", "Station response HTTP " + code + ", content type " + type);
                if (code != HttpURLConnection.HTTP_OK) throw new IllegalStateException("Station returned HTTP " + code + ".");
                if (type != null && (type.contains("mpegurl") || type.contains("x-mpegURL") || type.startsWith("text/")))
                    throw new IllegalStateException("Station returned a playlist instead of a direct audio stream.");
                feed.onConnected();
                listener.onStatus("Connected over Wi-Fi. Listening and learning new loops…");
                try (InputStream input = active.getInputStream()) {
                    byte[] bytes = new byte[8192]; int count;
                    while (running.get() && selectedNetwork.equals(network) && (count = input.read(bytes)) >= 0) {
                        if (count == 0) continue;
                        byte[] chunk = new byte[count]; System.arraycopy(bytes, 0, chunk, 0, count);
                        feed.onBytes(chunk);
                    }
                }
                if (running.get() && selectedNetwork.equals(network)) throw new IllegalStateException("The station stream ended.");
            } catch (Exception error) {
                Log.e("RadioMusicCapture", "Station stream failed", error);
                if (running.get() && selectedNetwork.equals(network)) {
                    listener.onStatus("Radio interrupted (" + error.getMessage() + "); reconnecting over Wi-Fi…");
                    try { Thread.sleep(2500); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
                }
            } finally {
                if (connection == active) connection = null;
                if (active != null) active.disconnect();
            }
        }
    }

    synchronized void stop() {
        if (!running.getAndSet(false)) return;
        network = null;
        if (connectivity != null && callback != null) {
            try { connectivity.unregisterNetworkCallback(callback); } catch (IllegalArgumentException ignored) {}
        }
        HttpURLConnection active = connection;
        if (active != null) active.disconnect();
        Thread thread = streamThread;
        if (thread != null) {
            thread.interrupt();
            try { thread.join(3000); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
        }
        streamThread = null; callback = null;
    }
}
