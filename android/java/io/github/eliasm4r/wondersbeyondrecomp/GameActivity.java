package io.github.eliasm4r.wondersbeyondrecomp;

import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;

public class GameActivity extends SDLActivity {
    private volatile int askedRate = -1;

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (mActivityCreated && !mSDLMainFinished) {
            try {
                nativeSetenv("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1");
            } catch (UnsatisfiedLinkError error) {
            }
        }
        super.onCreate(savedInstanceState);
        Updater.checkInBackground(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        final int rate = askedRate;
        if (rate > 0) {
            chooseMode(rate);
        }
    }

    public byte[] cloudRequest(String method, String address, byte[] body, long revision) {
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(address).openConnection();
            connection.setRequestMethod(method);
            connection.setConnectTimeout(3000);
            connection.setReadTimeout(4000);
            connection.setRequestProperty("Content-Type", "application/octet-stream");
            connection.setRequestProperty("X-Revision", Long.toString(revision));
            if (body != null) {
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(body.length);
                try (OutputStream out = connection.getOutputStream()) {
                    out.write(body);
                }
            }
            int status = connection.getResponseCode();
            String given = connection.getHeaderField("X-Revision");
            long seen = given == null ? 0 : Long.parseLong(given);
            ByteArrayOutputStream reply = new ByteArrayOutputStream();
            InputStream in = status < 400 ? connection.getInputStream() : connection.getErrorStream();
            if (in != null) {
                try (InputStream stream = in) {
                    byte[] buffer = new byte[8192];
                    int read;
                    while ((read = stream.read(buffer)) > 0) {
                        reply.write(buffer, 0, read);
                    }
                }
            }
            byte[] content = reply.toByteArray();
            return ByteBuffer.allocate(12 + content.length).putInt(status).putLong(seen).put(content).array();
        } catch (IOException | NumberFormatException problem) {
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    public int displayRate(final int wanted) {
        Display shown = getWindowManager().getDefaultDisplay();
        boolean dropped = shown != null && wanted > 60 && shown.getRefreshRate() < Math.min(wanted, highestRate()) - 1;
        if (wanted != askedRate || dropped) {
            askedRate = wanted;
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    chooseMode(wanted);
                }
            });
        }
        Display display = getWindowManager().getDefaultDisplay();
        return display == null ? 0 : Math.round(display.getRefreshRate());
    }

    public int highestRate() {
        Display display = getWindowManager().getDefaultDisplay();
        if (display == null) {
            return 0;
        }
        Display.Mode now = display.getMode();
        float best = now.getRefreshRate();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == now.getPhysicalWidth() && mode.getPhysicalHeight() == now.getPhysicalHeight()) {
                best = Math.max(best, mode.getRefreshRate());
            }
        }
        return Math.round(best);
    }

    private void chooseMode(int wanted) {
        Display display = getWindowManager().getDefaultDisplay();
        if (display == null || getWindow() == null) {
            return;
        }
        Display.Mode now = display.getMode();
        Display.Mode best = null;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() != now.getPhysicalWidth() || mode.getPhysicalHeight() != now.getPhysicalHeight()) {
                continue;
            }
            if (best == null) {
                best = mode;
                continue;
            }
            float rate = mode.getRefreshRate();
            float held = best.getRefreshRate();
            boolean fits = rate >= wanted - 1;
            boolean heldFits = held >= wanted - 1;
            if ((fits && (!heldFits || rate < held)) || (!fits && !heldFits && rate > held)) {
                best = mode;
            }
        }
        if (best == null) {
            return;
        }
        WindowManager.LayoutParams params = getWindow().getAttributes();
        if (params.preferredDisplayModeId != best.getModeId() || params.preferredRefreshRate != best.getRefreshRate()) {
            params.preferredDisplayModeId = best.getModeId();
            params.preferredRefreshRate = best.getRefreshRate();
            getWindow().setAttributes(params);
        }
        if (Build.VERSION.SDK_INT >= 31 && mSurface != null) {
            Surface surface = mSurface.getHolder().getSurface();
            if (surface != null && surface.isValid()) {
                try {
                    surface.setFrameRate(best.getRefreshRate(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, Surface.CHANGE_FRAME_RATE_ALWAYS);
                } catch (RuntimeException error) {
                }
            }
        }
    }
}
