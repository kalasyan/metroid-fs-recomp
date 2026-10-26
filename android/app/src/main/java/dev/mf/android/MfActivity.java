package dev.mf.android;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * Entry point: SDL's activity loads libSDL3.so and libmain.so (the game +
 * port, built from the repository's CMakeLists.txt) and calls SDL_main.
 *
 * It also drives the second screen: on devices with a secondary display
 * (the AYN Thor's lower screen) it shows a Presentation there with the area
 * map or Samus' status, drawn by the native side (port/src/second_screen.c)
 * and pulled through JNI (host_sdl.c). With a single screen, or with the
 * option off, it does nothing.
 */
public class MfActivity extends SDLActivity implements DisplayManager.DisplayListener {
    private static final String TAG = "mf";
    private static final long CHECK_INTERVAL_MS = 500;

    // host_sdl.c. What the second screen shows: 0 = off (or the game isn't
    // running yet), 1 = map, 2 = status.
    static native int nativeSecondScreenMode();
    // Canvas size for a display as (width << 16) | height, 0 when it's off.
    static native int nativeSecondScreenCanvas(int displayWidth, int displayHeight);
    // Copies the latest picture (ARGB); false if there is no new one.
    static native boolean nativeSecondScreenPixels(int[] pixels);

    private DisplayManager mDisplayManager;
    private Handler mHandler;
    private SecondScreen mSecondScreen;
    private boolean mStarted;
    private boolean mSecondScreenBroken;

    private final Runnable mCheckSecondScreen = new Runnable() {
        @Override
        public void run() {
            updateSecondScreen();
            if (mStarted) {
                mHandler.postDelayed(this, CHECK_INTERVAL_MS);
            }
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mHandler = new Handler(Looper.getMainLooper());
        mDisplayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
    }

    @Override
    protected void onStart() {
        super.onStart();
        mStarted = true;
        if (mDisplayManager != null && mHandler != null) {
            mDisplayManager.registerDisplayListener(this, mHandler);
            mHandler.post(mCheckSecondScreen);
        }
    }

    @Override
    protected void onStop() {
        mStarted = false;
        if (mDisplayManager != null && mHandler != null) {
            mDisplayManager.unregisterDisplayListener(this);
            mHandler.removeCallbacks(mCheckSecondScreen);
        }
        dismissSecondScreen();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        dismissSecondScreen();
        super.onDestroy();
    }

    @Override
    public void onDisplayAdded(int displayId) {
        updateSecondScreen();
    }

    @Override
    public void onDisplayRemoved(int displayId) {
        updateSecondScreen();
    }

    @Override
    public void onDisplayChanged(int displayId) {
        updateSecondScreen();
    }

    /** True when the game is running and wants something on the second screen. */
    private boolean secondScreenWanted() {
        if (mSecondScreenBroken || SDLActivity.mBrokenLibraries) {
            return false;
        }
        try {
            return nativeSecondScreenMode() != 0;
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "second screen: native code missing", e);
            mSecondScreenBroken = true;
            return false;
        }
    }

    private int currentDisplayId() {
        Display display = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display = getDisplay();
        } else {
            WindowManager wm = getWindowManager();
            if (wm != null) {
                display = wm.getDefaultDisplay();
            }
        }
        return display != null ? display.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    /** A display other than the game's: a presentation display first, then any public one. */
    private Display findSecondDisplay() {
        if (mDisplayManager == null) {
            return null;
        }
        final int current = currentDisplayId();
        for (Display d : mDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (d.getDisplayId() != current) {
                return d;
            }
        }
        for (Display d : mDisplayManager.getDisplays()) {
            if (d.getDisplayId() != current && (d.getFlags() & Display.FLAG_PRIVATE) == 0
                    && d.getState() != Display.STATE_OFF) {
                return d;
            }
        }
        return null;
    }

    private void updateSecondScreen() {
        if (!mStarted || isFinishing()) {
            dismissSecondScreen();
            return;
        }
        final Display display = secondScreenWanted() ? findSecondDisplay() : null;
        if (mSecondScreen != null
                && (display == null || mSecondScreen.getDisplay().getDisplayId() != display.getDisplayId())) {
            dismissSecondScreen();
        }
        if (display != null && mSecondScreen == null) {
            SecondScreen screen = new SecondScreen(this, display);
            try {
                screen.show();
                mSecondScreen = screen;
                Log.v(TAG, "second screen on display " + display.getDisplayId() + " (" + display.getName() + ")");
            } catch (RuntimeException e) {
                // WindowManager.InvalidDisplayException, or a display that
                // doesn't take presentations.
                Log.w(TAG, "second screen: can't use display " + display.getDisplayId(), e);
                mSecondScreenBroken = true;
            }
        }
    }

    private void dismissSecondScreen() {
        if (mSecondScreen != null) {
            try {
                mSecondScreen.dismiss();
            } catch (RuntimeException e) {
                Log.w(TAG, "second screen: dismiss failed", e);
            }
            mSecondScreen = null;
        }
    }

    /** The window on the secondary display. Never takes focus: the game keeps the input. */
    private static final class SecondScreen extends Presentation {
        SecondScreen(Context outerContext, Display display) {
            super(outerContext, display);
            setCancelable(false);
        }

        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            if (getWindow() != null) {
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
            setContentView(new ScreenView(getContext()));
        }
    }

    /** Pulls the native picture every display frame and scales it to fit. */
    private static final class ScreenView extends View {
        private final Paint mPaint = new Paint();
        private final Rect mDst = new Rect();
        private Bitmap mBitmap;
        private int[] mPixels;
        private boolean mAttached;

        private final Runnable mTick = new Runnable() {
            @Override
            public void run() {
                tick();
            }
        };

        ScreenView(Context context) {
            super(context);
            setBackgroundColor(Color.BLACK);
            mPaint.setFilterBitmap(false);
            mPaint.setAntiAlias(false);
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            mAttached = true;
            postOnAnimation(mTick);
        }

        @Override
        protected void onDetachedFromWindow() {
            mAttached = false;
            removeCallbacks(mTick);
            super.onDetachedFromWindow();
        }

        private void tick() {
            if (!mAttached) {
                return;
            }
            final int w = getWidth();
            final int h = getHeight();
            if (w > 0 && h > 0) {
                int size = 0;
                try {
                    size = nativeSecondScreenCanvas(w, h);
                } catch (UnsatisfiedLinkError e) {
                    return;
                }
                if (size != 0) {
                    final int cw = size >>> 16;
                    final int ch = size & 0xFFFF;
                    if (mBitmap == null || mBitmap.getWidth() != cw || mBitmap.getHeight() != ch) {
                        mBitmap = Bitmap.createBitmap(cw, ch, Bitmap.Config.ARGB_8888);
                        mPixels = new int[cw * ch];
                    }
                    if (nativeSecondScreenPixels(mPixels)) {
                        mBitmap.setPixels(mPixels, 0, cw, 0, 0, cw, ch);
                        invalidate();
                    }
                }
            }
            postOnAnimation(mTick);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (mBitmap == null) {
                return;
            }
            final int w = getWidth();
            final int h = getHeight();
            final int bw = mBitmap.getWidth();
            final int bh = mBitmap.getHeight();
            // Aspect-correct; whole pixels when they fit.
            float scale = Math.min((float) w / bw, (float) h / bh);
            if (scale >= 1.0f) {
                scale = (float) Math.floor(scale);
            }
            final int dw = Math.round(bw * scale);
            final int dh = Math.round(bh * scale);
            mDst.set((w - dw) / 2, (h - dh) / 2, (w - dw) / 2 + dw, (h - dh) / 2 + dh);
            canvas.drawBitmap(mBitmap, null, mDst, mPaint);
        }
    }
}
