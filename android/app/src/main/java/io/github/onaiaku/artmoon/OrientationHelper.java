package io.github.onaiaku.artmoon;

import android.app.Activity;

/**
 * Orientation policy: ArtMoon follows the device.
 *
 * The app imposes no orientation of its own, so it rotates exactly as the
 * phone does, subject to the system's auto-rotate setting — which belongs to
 * the user, not to us. When auto-rotate is off the system's own rotate button
 * appears, because nothing is pinning the activity any more.
 *
 * TVs are unaffected in practice: a Shield reports a single orientation and
 * never rotates, so landscape there is simply what the device already is.
 *
 * Spec: docs/android-ui-spec.md §1.
 */
public final class OrientationHelper {

    private OrientationHelper() {}

    /**
     * Call from every ArtMoon activity's onCreate AND from its
     * onConfigurationChanged.
     *
     * UNSPECIFIED is the "no opinion" value — the state an activity is in when
     * it declares nothing at all — so the system decides and the app follows
     * the device. Re-applying it on every configuration change is also what
     * releases a lock left behind by an older build, which is why this must run
     * on screen changes and not only on creation.
     */
    public static void applyOrientation(Activity activity) {
        activity.setRequestedOrientation(
                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
    }
}
