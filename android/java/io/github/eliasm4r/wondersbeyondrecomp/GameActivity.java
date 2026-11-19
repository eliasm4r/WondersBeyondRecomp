package io.github.eliasm4r.wondersbeyondrecomp;

import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

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
