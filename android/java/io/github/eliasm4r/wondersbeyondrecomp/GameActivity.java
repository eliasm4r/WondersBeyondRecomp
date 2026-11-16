package io.github.eliasm4r.wondersbeyondrecomp;

import android.os.Bundle;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
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
}
