package io.github.eliasm4r.wondersbeyondrecomp;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.system.Os;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewOutlineProvider;
import android.view.Window;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class SetupActivity extends Activity {
    private static final int PICK_DISC = 1;
    private static final String DISC_ID = "S2LP01";
    private static final long GAME_BYTES = 1515932406L;
    private static final long SPARE_BYTES = 120L << 20;
    private static final String[] FORMATS = {"ISO", "RVZ", "WBFS", "WIA", "GCZ", "CISO", "TGC", "GCM"};
    private static final Pattern EXTRACTING = Pattern.compile("^Extracting (.+) \\(size: ([0-9.]+) ?([KMGT]?i?B|bytes?)\\)");

    private static final int INK = 0xFF3B3531;
    private static final int INK_MUTED = 0xFF8D847C;
    private static final int PANEL = 0xFFF8F8F6;
    private static final int ORANGE = 0xFFF27D0C;
    private static final int ORANGE_LIGHT = 0xFFFFB46A;
    private static final int CHIP = 0xFFECE9E4;
    private static final int RULE = 0xFFE2DDD6;
    private static final int RED = 0xFFE2343B;
    private static final int GREEN = 0xFF3BA55C;

    private final Handler main = new Handler(Looper.getMainLooper());
    private Typeface black;
    private Typeface bold;
    private Map<String, String> text;
    private LinearLayout card;
    private File game;
    private volatile Process running;
    private volatile boolean cancelled;
    private Thread worker;
    private Thread previous;
    private Runnable back;
    private Updater.Release update;
    private boolean awaitingPermission;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        File base = getExternalFilesDir(null);
        if (base == null) {
            base = getFilesDir();
        }
        game = new File(base, "game");
        new File(getCacheDir(), "update.apk").delete();
        boolean installed = new File(game, "extracted/sys/main.dol").isFile();
        update = installed ? Updater.pending(this) : null;
        if (installed && update == null) {
            startGame();
            return;
        }
        text = texts();
        black = font("fonts/Nunito-Black.ttf", Typeface.BOLD);
        bold = font("fonts/Nunito-ExtraBold.ttf", Typeface.BOLD);
        Window window = getWindow();
        window.setBackgroundDrawable(new ColorDrawable(0xFF14213D));
        hideBars();
        setContentView(layout());
        if (update != null) {
            showUpdate(null);
        } else {
            showChoose(null);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (awaitingPermission && update != null) {
            awaitingPermission = false;
            if (mayInstall()) {
                downloadUpdate();
            } else {
                showUpdate(t("permission_refused"));
            }
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideBars();
        }
    }

    @Override
    protected void onDestroy() {
        cancel();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (worker != null) {
            cancel();
            if (back != null) {
                back.run();
            }
            return;
        }
        super.onBackPressed();
    }

    private void hideBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void startGame() {
        Intent intent = new Intent(this, GameActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        overridePendingTransition(0, 0);
        finish();
    }

    private Typeface font(String path, int fallback) {
        try {
            return Typeface.createFromAsset(getAssets(), path);
        } catch (RuntimeException error) {
            return Typeface.create(Typeface.SANS_SERIF, fallback);
        }
    }

    private int dp(float value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics()));
    }

    private String t(String key) {
        String value = text.get(key);
        return value != null ? value : key;
    }

    private View layout() {
        FrameLayout root = new FrameLayout(this);
        root.addView(new Backdrop(this), new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(false);
        root.addView(scroll, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        boolean wide = getResources().getDisplayMetrics().widthPixels > getResources().getDisplayMetrics().heightPixels;
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        row.setGravity(Gravity.CENTER);
        row.setPadding(dp(32), dp(20), dp(32), dp(20));
        scroll.addView(row, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout brand = new LinearLayout(this);
        brand.setOrientation(LinearLayout.VERTICAL);
        brand.setGravity(wide ? Gravity.START | Gravity.CENTER_VERTICAL : Gravity.CENTER_HORIZONTAL);
        brand.setPadding(0, 0, wide ? dp(36) : 0, wide ? 0 : dp(20));

        FrameLayout badge = new FrameLayout(this);
        GradientDrawable ring = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {0xFFFFFFFF, 0xFFFFE3C4});
        ring.setShape(GradientDrawable.OVAL);
        badge.setBackground(ring);
        badge.setElevation(dp(10));
        badge.setPadding(dp(8), dp(8), dp(8), dp(8));
        ImageView icon = new ImageView(this);
        icon.setImageResource(getApplicationInfo().icon);
        icon.setClipToOutline(true);
        icon.setOutlineProvider(new ViewOutlineProvider() {
            @Override
            public void getOutline(View view, Outline outline) {
                outline.setOval(0, 0, view.getWidth(), view.getHeight());
            }
        });
        badge.addView(icon, new FrameLayout.LayoutParams(dp(84), dp(84)));
        brand.addView(badge, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = label("WondersBeyond", black, 34, Color.WHITE);
        title.setSingleLine(true);
        title.setPadding(0, dp(14), 0, 0);
        title.setShadowLayer(dp(8), 0, dp(3), 0x55000000);
        float titleWidth = title.getPaint().measureText("WondersBeyond");
        title.getPaint().setShader(new LinearGradient(0, 0, titleWidth, 0, new int[] {Color.WHITE, ORANGE_LIGHT, ORANGE}, null, Shader.TileMode.CLAMP));
        brand.addView(title);
        TextView subtitle = label("R E C O M P", black, 15, ORANGE_LIGHT);
        subtitle.setSingleLine(true);
        subtitle.setLetterSpacing(0.35f);
        subtitle.setPadding(dp(14), dp(3), dp(14), dp(3));
        GradientDrawable pill = new GradientDrawable();
        pill.setCornerRadius(dp(20));
        pill.setStroke(dp(2), ORANGE_LIGHT);
        subtitle.setBackground(pill);
        LinearLayout.LayoutParams pillPlace = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        pillPlace.topMargin = dp(6);
        pillPlace.gravity = wide ? Gravity.START : Gravity.CENTER_HORIZONTAL;
        brand.addView(subtitle, pillPlace);
        TextView tagline = label(t("tagline"), bold, 14, 0xCCFFFFFF);
        tagline.setPadding(0, dp(8), 0, 0);
        tagline.setMaxWidth(dp(300));
        if (!wide) {
            tagline.setGravity(Gravity.CENTER_HORIZONTAL);
        }
        brand.addView(tagline);
        row.addView(brand, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(26), dp(24), dp(26), dp(22));
        GradientDrawable panel = new GradientDrawable();
        panel.setColor(PANEL);
        panel.setCornerRadius(dp(26));
        card.setBackground(panel);
        card.setElevation(dp(18));
        LinearLayout.LayoutParams cardSize = new LinearLayout.LayoutParams(Math.min(dp(470), getResources().getDisplayMetrics().widthPixels - dp(64)),
                ViewGroup.LayoutParams.WRAP_CONTENT);
        row.addView(card, cardSize);

        card.setAlpha(0f);
        card.setTranslationY(dp(24));
        card.animate().alpha(1f).translationY(0).setDuration(420).setStartDelay(80).setInterpolator(new DecelerateInterpolator()).start();
        brand.setAlpha(0f);
        brand.animate().alpha(1f).setDuration(500).start();
        return root;
    }

    private TextView label(String value, Typeface face, float size, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTypeface(face);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, size);
        view.setTextColor(color);
        view.setIncludeFontPadding(false);
        return view;
    }

    private TextView heading(String value) {
        TextView view = label(value, black, 24, INK);
        card.addView(view);
        return view;
    }

    private TextView body(String value, int color, int top) {
        TextView view = label(value, bold, 14.5f, color);
        view.setLineSpacing(dp(3), 1f);
        view.setPadding(0, dp(top), 0, 0);
        card.addView(view);
        return view;
    }

    private TextView button(String value, boolean primary, View.OnClickListener click) {
        TextView view = label(value, black, 17, primary ? Color.WHITE : ORANGE);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(20), dp(15), dp(20), dp(15));
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(40));
        if (primary) {
            shape.setColors(new int[] {0xFFF59A40, ORANGE});
            shape.setOrientation(GradientDrawable.Orientation.TOP_BOTTOM);
            view.setElevation(dp(4));
        } else {
            shape.setColor(0x00FFFFFF);
            shape.setStroke(dp(2), RULE);
        }
        GradientDrawable mask = new GradientDrawable();
        mask.setCornerRadius(dp(40));
        mask.setColor(Color.WHITE);
        view.setBackground(new RippleDrawable(ColorStateList.valueOf(primary ? 0x55FFFFFF : 0x22F27D0C), shape, mask));
        view.setClickable(true);
        view.setFocusable(true);
        view.setOnClickListener(click);
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        size.topMargin = dp(primary ? 20 : 10);
        card.addView(view, size);
        return view;
    }

    private void refill(Runnable content) {
        card.animate().cancel();
        card.setAlpha(0.35f);
        card.removeAllViews();
        content.run();
        card.animate().alpha(1f).setDuration(220).start();
    }

    private void showChoose(String problem) {
        worker = null;
        refill(new Runnable() {
            @Override
            public void run() {
                if (problem == null) {
                    heading(t("choose_title"));
                    body(t("choose_body"), INK_MUTED, 10);
                } else {
                    TextView title = heading(t("error_title"));
                    title.setTextColor(RED);
                    TextView message = body(problem, INK, 12);
                    GradientDrawable box = new GradientDrawable();
                    box.setColor(0x14E2343B);
                    box.setCornerRadius(dp(14));
                    message.setBackground(box);
                    message.setPadding(dp(14), dp(12), dp(14), dp(12));
                    ((LinearLayout.LayoutParams) message.getLayoutParams()).topMargin = dp(12);
                }
                Flow chips = new Flow(SetupActivity.this, dp(6));
                for (String format : FORMATS) {
                    TextView chip = label(format, black, 12, INK_MUTED);
                    chip.setLetterSpacing(0.06f);
                    chip.setPadding(dp(11), dp(6), dp(11), dp(6));
                    GradientDrawable pill = new GradientDrawable();
                    pill.setColor(CHIP);
                    pill.setCornerRadius(dp(20));
                    chip.setBackground(pill);
                    chips.addView(chip);
                }
                LinearLayout.LayoutParams chipSpace = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                chipSpace.topMargin = dp(16);
                card.addView(chips, chipSpace);
                button(t(problem == null ? "choose_button" : "retry"), true, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        pick();
                    }
                });
                body(t("choose_note"), INK_MUTED, 14).setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
            }
        });
    }

    private TextView percent;
    private Bar bar;
    private TextView status;

    private void showWorking(final String title, final String name, final Runnable cancelled) {
        back = cancelled;
        refill(new Runnable() {
            @Override
            public void run() {
                heading(title);
                TextView file = body(name, INK_MUTED, 6);
                file.setSingleLine(true);
                file.setEllipsize(TextUtils.TruncateAt.MIDDLE);
                percent = label("0 %", black, 44, ORANGE);
                percent.setPadding(0, dp(16), 0, dp(6));
                card.addView(percent);
                bar = new Bar(SetupActivity.this);
                card.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(14)));
                status = body(t("preparing"), INK_MUTED, 10);
                status.setSingleLine(true);
                status.setEllipsize(TextUtils.TruncateAt.END);
                button(t("cancel"), false, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        cancel();
                        cancelled.run();
                    }
                }).setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            }
        });
    }

    private void showDone() {
        worker = null;
        refill(new Runnable() {
            @Override
            public void run() {
                View check = new Check(SetupActivity.this);
                LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(56), dp(56));
                size.bottomMargin = dp(12);
                card.addView(check, size);
                heading(t("done_title"));
                body(t("done_body"), INK_MUTED, 10);
                button(t("play"), true, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        startGame();
                    }
                });
            }
        });
    }

    private void showUpdate(final String problem) {
        worker = null;
        refill(new Runnable() {
            @Override
            public void run() {
                if (problem == null) {
                    TextView tag = label(t("update_badge"), black, 12, ORANGE);
                    tag.setLetterSpacing(0.08f);
                    tag.setPadding(dp(12), dp(6), dp(12), dp(6));
                    GradientDrawable pill = new GradientDrawable();
                    pill.setColor(0x1FF27D0C);
                    pill.setCornerRadius(dp(20));
                    tag.setBackground(pill);
                    LinearLayout.LayoutParams place = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                    place.bottomMargin = dp(12);
                    card.addView(tag, place);
                    heading(String.format(t("update_title"), update.tag));
                    body(String.format(t("update_body"), Updater.installedVersion(SetupActivity.this)), INK_MUTED, 10);
                } else {
                    TextView title = heading(t("error_title"));
                    title.setTextColor(RED);
                    TextView message = body(problem.isEmpty() ? t("update_failed") : problem, INK, 12);
                    GradientDrawable box = new GradientDrawable();
                    box.setColor(0x14E2343B);
                    box.setCornerRadius(dp(14));
                    message.setBackground(box);
                    message.setPadding(dp(14), dp(12), dp(14), dp(12));
                }
                button(t(problem == null ? "update_button" : "retry_update"), true, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        beginUpdate();
                    }
                });
                button(t("later"), false, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        startGame();
                    }
                }).setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
                body(String.format(t("update_note"), update.size > 0 ? String.valueOf(Math.max(1, Math.round(update.size / 1e6))) : "20"), INK_MUTED, 14).setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
            }
        });
    }

    private boolean mayInstall() {
        return getPackageManager().canRequestPackageInstalls();
    }

    private void beginUpdate() {
        if (mayInstall()) {
            downloadUpdate();
            return;
        }
        refill(new Runnable() {
            @Override
            public void run() {
                heading(t("permission_title"));
                body(t("permission_body"), INK_MUTED, 10);
                button(t("permission_button"), true, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        awaitingPermission = true;
                        try {
                            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
                        } catch (RuntimeException error) {
                            awaitingPermission = false;
                            showUpdate(t("permission_refused"));
                        }
                    }
                });
                button(t("later"), false, new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        startGame();
                    }
                }).setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
            }
        });
    }

    private void downloadUpdate() {
        if (previous != null) {
            try {
                previous.join(4000);
            } catch (InterruptedException error) {
            }
        }
        cancelled = false;
        final Updater.Release release = update;
        showWorking(t("download_title"), release.tag, new Runnable() {
            @Override
            public void run() {
                showUpdate(null);
            }
        });
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                File apk = new File(getCacheDir(), "update.apk");
                try {
                    boolean done = Updater.download(release, apk, new Updater.Progress() {
                        @Override
                        public boolean step(long got, long total) {
                            if (total > 0) {
                                progress((double) got / total, String.format(Locale.getDefault(), "%.1f / %.1f MB", got / 1e6, total / 1e6));
                            }
                            return !cancelled;
                        }
                    });
                    if (!done || cancelled) {
                        apk.delete();
                        return;
                    }
                    progress(1, t("installing"));
                    Updater.install(SetupActivity.this, apk, new Updater.Outcome() {
                        @Override
                        public void finished(final boolean installing, final String problem) {
                            main.post(new Runnable() {
                                @Override
                                public void run() {
                                    if (installing) {
                                        if (status != null) {
                                            status.setText(t("confirm_install"));
                                        }
                                    } else {
                                        showUpdate(problem);
                                    }
                                }
                            });
                        }
                    });
                } catch (Exception error) {
                    apk.delete();
                    if (!cancelled) {
                        main.post(new Runnable() {
                            @Override
                            public void run() {
                                showUpdate(t("update_offline"));
                            }
                        });
                    }
                }
            }
        }, "update download");
        previous = worker;
        worker.start();
    }

    private void progress(double fraction, String line) {
        main.post(new Runnable() {
            @Override
            public void run() {
                if (bar == null || worker == null) {
                    return;
                }
                double value = Math.max(0, Math.min(1, fraction));
                bar.set((float) value);
                percent.setText(String.format(Locale.ROOT, "%d %%", (int) Math.floor(value * 100)));
                if (line != null) {
                    status.setText(line);
                }
            }
        });
    }

    private void fail(String message) {
        main.post(new Runnable() {
            @Override
            public void run() {
                if (!cancelled) {
                    showChoose(message);
                }
            }
        });
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        try {
            startActivityForResult(intent, PICK_DISC);
        } catch (RuntimeException error) {
            Intent fallback = new Intent(Intent.ACTION_GET_CONTENT);
            fallback.addCategory(Intent.CATEGORY_OPENABLE);
            fallback.setType("*/*");
            try {
                startActivityForResult(Intent.createChooser(fallback, t("choose_button")), PICK_DISC);
            } catch (RuntimeException none) {
                showChoose(t("no_picker"));
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_DISC || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        String name = displayName(uri);
        long size = displaySize(uri);
        if (previous != null) {
            try {
                previous.join(4000);
            } catch (InterruptedException error) {
            }
        }
        cancelled = false;
        showWorking(t("working_title"), name, new Runnable() {
            @Override
            public void run() {
                showChoose(null);
            }
        });
        worker = new Thread(new Runnable() {
            @Override
            public void run() {
                install(uri, name, size);
            }
        }, "disc setup");
        previous = worker;
        worker.start();
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        } catch (RuntimeException error) {
        }
        String path = uri.getLastPathSegment();
        return path != null ? path : "disc";
    }

    private long displaySize(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getLong(0);
            }
        } catch (RuntimeException error) {
        }
        return -1;
    }

    private void cancel() {
        cancelled = true;
        Process process = running;
        if (process != null) {
            process.destroy();
        }
        worker = null;
    }

    private void install(Uri uri, String name, long size) {
        File staging = new File(game, "extracting");
        File copy = new File(getCacheDir(), "disc" + extension(name));
        try {
            game.mkdirs();
            delete(staging);
            delete(copy);
            long free = new StatFs(game.getPath()).getAvailableBytes();
            if (free < GAME_BYTES + SPARE_BYTES) {
                fail(String.format(t("no_space"), gigabytes(GAME_BYTES + SPARE_BYTES - free)));
                return;
            }
            File tool = new File(getApplicationInfo().nativeLibraryDir, "libnodtool.so");
            if (!tool.isFile()) {
                fail(t("no_tool"));
                return;
            }
            Result result;
            try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(uri, "r")) {
                if (descriptor == null) {
                    fail(t("cannot_open"));
                    return;
                }
                result = null;
                for (String path : directPaths(descriptor)) {
                    result = extract(tool, path, staging, 0, 1);
                    if (cancelled || result.started || !result.tail.contains("os error")) {
                        break;
                    }
                }
            }
            if (cancelled) {
                return;
            }
            if (result == null || !result.started && result.tail.contains("os error")) {
                long room = new StatFs(getCacheDir().getPath()).getAvailableBytes();
                if (size > 0 && room < size + GAME_BYTES + SPARE_BYTES) {
                    fail(String.format(t("no_space"), gigabytes(size + GAME_BYTES + SPARE_BYTES - room)));
                    return;
                }
                if (!copyDisc(uri, copy, size)) {
                    return;
                }
                result = extract(tool, copy.getPath(), staging, 0.4, 0.6);
                delete(copy);
            }
            if (cancelled) {
                return;
            }
            if (result.code != 0) {
                fail(t("failed") + (result.tail.isEmpty() ? "" : "\n\n" + result.tail));
                delete(staging);
                return;
            }
            progress(1, t("checking"));
            String found = discId(new File(staging, "sys/boot.bin"));
            if (!DISC_ID.equals(found)) {
                delete(staging);
                fail(String.format(t("wrong_disc"), found == null ? "?" : found));
                return;
            }
            File target = new File(game, "extracted");
            File old = new File(game, "extracted.old");
            delete(old);
            if (target.exists() && !target.renameTo(old)) {
                delete(staging);
                fail(t("failed"));
                return;
            }
            if (!staging.renameTo(target)) {
                old.renameTo(target);
                delete(staging);
                fail(t("failed"));
                return;
            }
            delete(old);
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (!cancelled) {
                        showDone();
                    }
                }
            });
        } catch (Exception error) {
            delete(staging);
            delete(copy);
            if (!cancelled) {
                fail(t("cannot_open") + "\n\n" + error.getMessage());
            }
        } finally {
            if (cancelled) {
                delete(staging);
                delete(copy);
            }
        }
    }

    private String[] directPaths(ParcelFileDescriptor descriptor) {
        String own = "/proc/" + android.os.Process.myPid() + "/fd/" + descriptor.getFd();
        try {
            String real = Os.readlink("/proc/self/fd/" + descriptor.getFd());
            if (real.startsWith("/") && new File(real).canRead()) {
                return new String[] {real, own};
            }
        } catch (Exception error) {
        }
        return new String[] {own};
    }

    private static class Result {
        int code = -1;
        boolean started;
        String tail = "";
    }

    private Result extract(File tool, String input, File output, double start, double share) throws Exception {
        Result result = new Result();
        delete(output);
        ProcessBuilder builder = new ProcessBuilder(tool.getPath(), "--no-color", "extract", input, output.getPath());
        builder.redirectErrorStream(true);
        builder.directory(game);
        Process process = builder.start();
        running = process;
        if (cancelled) {
            process.destroy();
        }
        ArrayDeque<String> tail = new ArrayDeque<>();
        double done = 0;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), "UTF-8"))) {
            StringBuilder line = new StringBuilder();
            int character;
            while ((character = reader.read()) != -1) {
                if (character != '\n' && character != '\r') {
                    line.append((char) character);
                    continue;
                }
                if (line.length() == 0) {
                    continue;
                }
                String seen = line.toString().trim();
                line.setLength(0);
                Matcher match = EXTRACTING.matcher(seen);
                if (match.find()) {
                    result.started = true;
                    done += bytes(match.group(2), match.group(3));
                    String path = match.group(1).replace('\\', '/');
                    progress(start + share * Math.min(0.99, done / GAME_BYTES), path.substring(path.lastIndexOf('/') + 1));
                } else {
                    tail.addLast(seen);
                    if (tail.size() > 6) {
                        tail.removeFirst();
                    }
                }
            }
        }
        result.code = process.waitFor();
        running = null;
        result.tail = TextUtils.join("\n", tail);
        return result;
    }

    private static double bytes(String number, String unit) {
        double value;
        try {
            value = Double.parseDouble(number);
        } catch (NumberFormatException error) {
            return 0;
        }
        switch (unit.charAt(0)) {
            case 'K':
                    return value * 1e3;
            case 'M':
                    return value * 1e6;
            case 'G':
                    return value * 1e9;
            case 'T':
                    return value * 1e12;
            default:
                    return value;
        }
    }

    private boolean copyDisc(Uri uri, File copy, long size) throws Exception {
        progress(0, t("copying"));
        try (InputStream input = getContentResolver().openInputStream(uri); OutputStream output = new FileOutputStream(copy)) {
            if (input == null) {
                fail(t("cannot_open"));
                return false;
            }
            byte[] buffer = new byte[1 << 20];
            long copied = 0;
            int count;
            while ((count = input.read(buffer)) > 0) {
                if (cancelled) {
                    return false;
                }
                output.write(buffer, 0, count);
                copied += count;
                if (size > 0) {
                    progress(0.4 * copied / size, t("copying"));
                }
            }
        }
        return !cancelled;
    }

    private static String discId(File boot) {
        try (FileInputStream input = new FileInputStream(boot)) {
            byte[] id = new byte[6];
            if (input.read(id) != 6) {
                return null;
            }
            return new String(id, "US-ASCII");
        } catch (Exception error) {
            return null;
        }
    }

    private static String extension(String name) {
        int dot = name.lastIndexOf('.');
        String value = dot >= 0 ? name.substring(dot).toLowerCase(Locale.ROOT) : "";
        return value.matches("\\.[a-z0-9]{1,5}") ? value : ".iso";
    }

    private static String gigabytes(long count) {
        return String.format(Locale.getDefault(), "%.1f", Math.max(0.1, count / 1e9));
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                delete(child);
            }
        }
        file.delete();
    }

    private static class Backdrop extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float phase;

        Backdrop(Context context) {
            super(context);
            ValueAnimator drift = ValueAnimator.ofFloat(0f, (float) (Math.PI * 2));
            drift.setDuration(26000);
            drift.setRepeatCount(ValueAnimator.INFINITE);
            drift.setInterpolator(null);
            drift.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator animation) {
                    phase = (float) animation.getAnimatedValue();
                    invalidate();
                }
            });
            drift.start();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            int width = getWidth();
            int height = getHeight();
            paint.setShader(new LinearGradient(0, 0, width, height, new int[] {0xFF14213D, 0xFF1D3B53, 0xFF24504F}, null, Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, width, height, paint);
            glow(canvas, width * (0.16f + 0.03f * (float) Math.sin(phase)), height * (0.2f + 0.04f * (float) Math.cos(phase)), height * 0.75f, 0x55F27D0C);
            glow(canvas, width * (0.88f + 0.03f * (float) Math.cos(phase)), height * (0.86f + 0.04f * (float) Math.sin(phase)), height * 0.85f, 0x443AAFA9);
            glow(canvas, width * (0.62f + 0.05f * (float) Math.sin(phase * 2)), height * 0.08f, height * 0.45f, 0x33FFD166);
            paint.setShader(null);
            for (int i = 0; i < 46; i++) {
                float x = (float) ((i * 0.618034 + 0.13) % 1.0) * width;
                float y = (float) ((i * 0.754877 + 0.41) % 1.0) * height;
                float twinkle = 0.5f + 0.5f * (float) Math.sin(phase * 3 + i * 1.7);
                paint.setColor(Color.argb((int) (40 + 90 * twinkle), 255, 255, 255));
                canvas.drawCircle(x, y, (i % 3 == 0 ? 2.2f : 1.4f) * getResources().getDisplayMetrics().density, paint);
            }
        }

        private void glow(Canvas canvas, float x, float y, float radius, int color) {
            paint.setShader(new RadialGradient(x, y, radius, color, color & 0x00FFFFFF, Shader.TileMode.CLAMP));
            canvas.drawCircle(x, y, radius, paint);
        }
    }

    private static class Bar extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF shape = new RectF();
        private float shown;
        private float target;
        private ValueAnimator easing;

        Bar(Context context) {
            super(context);
        }

        void set(float value) {
            if (value <= target) {
                return;
            }
            if (easing != null) {
                easing.cancel();
            }
            target = value;
            easing = ValueAnimator.ofFloat(shown, value);
            easing.setDuration(260);
            easing.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator animation) {
                    shown = (float) animation.getAnimatedValue();
                    invalidate();
                }
            });
            easing.start();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float radius = getHeight() / 2f;
            paint.setShader(null);
            paint.setColor(CHIP);
            shape.set(0, 0, getWidth(), getHeight());
            canvas.drawRoundRect(shape, radius, radius, paint);
            if (shown <= 0) {
                return;
            }
            float end = Math.max(getHeight(), getWidth() * shown);
            paint.setShader(new LinearGradient(0, 0, end, 0, 0xFFFFB46A, ORANGE, Shader.TileMode.CLAMP));
            shape.set(0, 0, end, getHeight());
            canvas.drawRoundRect(shape, radius, radius, paint);
        }
    }

    private static class Check extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float drawn;

        Check(Context context) {
            super(context);
            ValueAnimator grow = ValueAnimator.ofFloat(0f, 1f);
            grow.setDuration(520);
            grow.setInterpolator(new DecelerateInterpolator());
            grow.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override
                public void onAnimationUpdate(ValueAnimator animation) {
                    drawn = (float) animation.getAnimatedValue();
                    invalidate();
                }
            });
            grow.start();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float size = Math.min(getWidth(), getHeight());
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(GREEN);
            canvas.drawCircle(size / 2, size / 2, size / 2 * Math.min(1f, drawn * 1.6f), paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(size * 0.1f);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
            paint.setColor(Color.WHITE);
            float step = Math.max(0f, Math.min(1f, (drawn - 0.35f) / 0.65f));
            float ax = size * 0.28f, ay = size * 0.52f, bx = size * 0.44f, by = size * 0.68f, cx = size * 0.74f, cy = size * 0.36f;
            android.graphics.Path path = new android.graphics.Path();
            path.moveTo(ax, ay);
            if (step < 0.4f) {
                float part = step / 0.4f;
                path.lineTo(ax + (bx - ax) * part, ay + (by - ay) * part);
            } else {
                float part = (step - 0.4f) / 0.6f;
                path.lineTo(bx, by);
                path.lineTo(bx + (cx - bx) * part, by + (cy - by) * part);
            }
            if (step > 0) {
                canvas.drawPath(path, paint);
            }
        }
    }

    private static class Flow extends ViewGroup {
        private final int gap;

        Flow(Context context, int gap) {
            super(context);
            this.gap = gap;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int limit = MeasureSpec.getSize(widthSpec);
            int x = 0;
            int y = 0;
            int line = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                child.measure(MeasureSpec.makeMeasureSpec(limit, MeasureSpec.AT_MOST), MeasureSpec.UNSPECIFIED);
                if (x > 0 && x + child.getMeasuredWidth() > limit) {
                    x = 0;
                    y += line + gap;
                    line = 0;
                }
                x += child.getMeasuredWidth() + gap;
                line = Math.max(line, child.getMeasuredHeight());
            }
            setMeasuredDimension(limit, y + line);
        }

        @Override
        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            int limit = right - left;
            int x = 0;
            int y = 0;
            int line = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                if (x > 0 && x + child.getMeasuredWidth() > limit) {
                    x = 0;
                    y += line + gap;
                    line = 0;
                }
                child.layout(x, y, x + child.getMeasuredWidth(), y + child.getMeasuredHeight());
                x += child.getMeasuredWidth() + gap;
                line = Math.max(line, child.getMeasuredHeight());
            }
        }
    }

    private static Map<String, String> texts() {
        String language = Locale.getDefault().getLanguage();
        String[][] chosen = language.equals("fr") ? FRENCH : language.equals("es") ? SPANISH : language.equals("gl") ? GALICIAN : ENGLISH;
        Map<String, String> map = new HashMap<>();
        for (String[] pair : ENGLISH) {
            map.put(pair[0], pair[1]);
        }
        for (String[] pair : chosen) {
            map.put(pair[0], pair[1]);
        }
        return map;
    }

    private static final String[][] ENGLISH = {
        {"tagline", "A native version of the Wii game, rebuilt for your phone."},
        {"choose_title", "Choose your game"},
        {"choose_body", "Pick your copy of PokéPark 2: Wonders Beyond (European version). The file can be anywhere: Downloads, a folder of your own or an SD card."},
        {"choose_button", "Choose the game file"},
        {"choose_note", "The game is unpacked once into the app's storage (about 1.6 GB). Your file is left untouched."},
        {"working_title", "Installing"},
        {"preparing", "Opening the file…"},
        {"copying", "Reading the file…"},
        {"checking", "Checking the game…"},
        {"cancel", "Cancel"},
        {"done_title", "All set!"},
        {"done_body", "The game is ready. Next time, it starts straight away."},
        {"play", "Play"},
        {"error_title", "That did not work"},
        {"retry", "Choose another file"},
        {"no_space", "Not enough free space: free up about %s GB, then try again."},
        {"no_tool", "The disc reader is missing from this build. Install the latest APK."},
        {"cannot_open", "This file cannot be opened. Copy it to the phone's storage (Downloads for example) and choose it again."},
        {"failed", "This file is not a readable Wii disc image, or it is damaged."},
        {"wrong_disc", "This disc is %s. The game needs PokéPark 2: Wonders Beyond, European version (S2LP01)."},
        {"no_picker", "No file picker was found on this phone."},
        {"update_badge", "UPDATE"},
        {"update_title", "Version %s is out"},
        {"update_body", "You have version %s. The update installs over this one: the game files and your saves stay as they are."},
        {"update_button", "Update now"},
        {"retry_update", "Try again"},
        {"later", "Later"},
        {"update_note", "About %s MB to download. Android asks you to confirm the install."},
        {"permission_title", "One permission first"},
        {"permission_body", "Android needs your go-ahead once so that the game can install its own updates. Turn on \"Allow from this source\", then come back."},
        {"permission_button", "Open the setting"},
        {"permission_refused", "The permission is still off, so the update cannot be installed. You can turn it on and try again."},
        {"download_title", "Downloading"},
        {"installing", "Installing…"},
        {"confirm_install", "Confirm the update on the screen Android shows."},
        {"update_failed", "Android did not install the update."},
        {"update_offline", "The update could not be downloaded. Check the internet connection and try again."},
    };

    private static final String[][] FRENCH = {
        {"tagline", "Une version native du jeu Wii, refaite pour ton téléphone."},
        {"choose_title", "Choisis ton jeu"},
        {"choose_body", "Sélectionne ta copie de PokéPark 2 : Wonders Beyond (version européenne). Le fichier peut être n'importe où : Téléchargements, un dossier à toi ou une carte SD."},
        {"choose_button", "Choisir le fichier du jeu"},
        {"choose_note", "Le jeu est décompressé une seule fois dans le stockage de l'appli (environ 1,6 Go). Ton fichier n'est pas modifié."},
        {"working_title", "Installation"},
        {"preparing", "Ouverture du fichier…"},
        {"copying", "Lecture du fichier…"},
        {"checking", "Vérification du jeu…"},
        {"cancel", "Annuler"},
        {"done_title", "C'est prêt !"},
        {"done_body", "Le jeu est installé. La prochaine fois, il se lancera directement."},
        {"play", "Jouer"},
        {"error_title", "Ça n'a pas marché"},
        {"retry", "Choisir un autre fichier"},
        {"no_space", "Pas assez de place : libère environ %s Go, puis réessaie."},
        {"no_tool", "Le lecteur de disque manque dans cette version. Installe le dernier APK."},
        {"cannot_open", "Ce fichier ne peut pas être ouvert. Copie-le dans le stockage du téléphone (Téléchargements par exemple) et choisis-le à nouveau."},
        {"failed", "Ce fichier n'est pas une image de disque Wii lisible, ou il est abîmé."},
        {"wrong_disc", "Ce disque est %s. Il faut PokéPark 2 : Wonders Beyond, version européenne (S2LP01)."},
        {"no_picker", "Aucun sélecteur de fichiers n'a été trouvé sur ce téléphone."},
        {"update_badge", "MISE À JOUR"},
        {"update_title", "La version %s est sortie"},
        {"update_body", "Tu as la version %s. La mise à jour s'installe par-dessus : les fichiers du jeu et tes sauvegardes restent en place."},
        {"update_button", "Mettre à jour"},
        {"retry_update", "Réessayer"},
        {"later", "Plus tard"},
        {"update_note", "Environ %s Mo à télécharger. Android te demande de confirmer l'installation."},
        {"permission_title", "Une autorisation d'abord"},
        {"permission_body", "Android doit te demander une seule fois ton accord pour que le jeu installe ses propres mises à jour. Active \"Autoriser cette source\", puis reviens."},
        {"permission_button", "Ouvrir le réglage"},
        {"permission_refused", "L'autorisation est toujours désactivée, la mise à jour ne peut pas s'installer. Tu peux l'activer et réessayer."},
        {"download_title", "Téléchargement"},
        {"installing", "Installation…"},
        {"confirm_install", "Confirme la mise à jour sur l'écran d'Android."},
        {"update_failed", "Android n'a pas installé la mise à jour."},
        {"update_offline", "La mise à jour n'a pas pu être téléchargée. Vérifie la connexion internet et réessaie."},
    };

    private static final String[][] SPANISH = {
        {"tagline", "Una versión nativa del juego de Wii, rehecha para tu móvil."},
        {"choose_title", "Elige tu juego"},
        {"choose_body", "Selecciona tu copia de PokéPark 2: Wonders Beyond (versión europea). El archivo puede estar en cualquier sitio: Descargas, una carpeta tuya o una tarjeta SD."},
        {"choose_button", "Elegir el archivo del juego"},
        {"choose_note", "El juego se descomprime una sola vez en el almacenamiento de la app (unos 1,6 GB). Tu archivo no se modifica."},
        {"working_title", "Instalando"},
        {"preparing", "Abriendo el archivo…"},
        {"copying", "Leyendo el archivo…"},
        {"checking", "Comprobando el juego…"},
        {"cancel", "Cancelar"},
        {"done_title", "¡Listo!"},
        {"done_body", "El juego está instalado. La próxima vez se abrirá directamente."},
        {"play", "Jugar"},
        {"error_title", "No ha funcionado"},
        {"retry", "Elegir otro archivo"},
        {"no_space", "No hay espacio suficiente: libera unos %s GB y vuelve a intentarlo."},
        {"no_tool", "Falta el lector de discos en esta versión. Instala el último APK."},
        {"cannot_open", "No se puede abrir este archivo. Cópialo al almacenamiento del móvil (Descargas, por ejemplo) y vuelve a elegirlo."},
        {"failed", "Este archivo no es una imagen de disco de Wii legible, o está dañado."},
        {"wrong_disc", "Este disco es %s. El juego necesita PokéPark 2: Wonders Beyond, versión europea (S2LP01)."},
        {"no_picker", "No se ha encontrado ningún selector de archivos en este móvil."},
        {"update_badge", "ACTUALIZACIÓN"},
        {"update_title", "Ya está la versión %s"},
        {"update_body", "Tienes la versión %s. La actualización se instala encima: los archivos del juego y tus partidas se quedan como están."},
        {"update_button", "Actualizar"},
        {"retry_update", "Reintentar"},
        {"later", "Más tarde"},
        {"update_note", "Unos %s MB de descarga. Android te pide que confirmes la instalación."},
        {"permission_title", "Primero, un permiso"},
        {"permission_body", "Android necesita tu permiso una sola vez para que el juego instale sus propias actualizaciones. Activa \"Permitir de esta fuente\" y vuelve."},
        {"permission_button", "Abrir el ajuste"},
        {"permission_refused", "El permiso sigue desactivado, así que la actualización no se puede instalar. Puedes activarlo y volver a intentarlo."},
        {"download_title", "Descargando"},
        {"installing", "Instalando…"},
        {"confirm_install", "Confirma la actualización en la pantalla de Android."},
        {"update_failed", "Android no ha instalado la actualización."},
        {"update_offline", "No se ha podido descargar la actualización. Comprueba la conexión a internet y vuelve a intentarlo."},
    };

    private static final String[][] GALICIAN = {
        {"tagline", "Unha versión nativa do xogo de Wii, refeita para o teu móbil."},
        {"choose_title", "Escolle o teu xogo"},
        {"choose_body", "Selecciona a túa copia de PokéPark 2: Wonders Beyond (versión europea). O ficheiro pode estar en calquera sitio: Descargas, un cartafol teu ou unha tarxeta SD."},
        {"choose_button", "Escoller o ficheiro do xogo"},
        {"choose_note", "O xogo descomprímese unha soa vez no almacenamento da app (uns 1,6 GB). O teu ficheiro non se modifica."},
        {"working_title", "Instalando"},
        {"preparing", "Abrindo o ficheiro…"},
        {"copying", "Lendo o ficheiro…"},
        {"checking", "Comprobando o xogo…"},
        {"cancel", "Cancelar"},
        {"done_title", "Listo!"},
        {"done_body", "O xogo está instalado. A próxima vez abrirase directamente."},
        {"play", "Xogar"},
        {"error_title", "Non funcionou"},
        {"retry", "Escoller outro ficheiro"},
        {"no_space", "Non hai espazo abondo: libera uns %s GB e téntao de novo."},
        {"no_tool", "Falta o lector de discos nesta versión. Instala o último APK."},
        {"cannot_open", "Non se pode abrir este ficheiro. Cópiao ao almacenamento do móbil (Descargas, por exemplo) e escóllelo de novo."},
        {"failed", "Este ficheiro non é unha imaxe de disco de Wii lexible, ou está danado."},
        {"wrong_disc", "Este disco é %s. O xogo necesita PokéPark 2: Wonders Beyond, versión europea (S2LP01)."},
        {"no_picker", "Non se atopou ningún selector de ficheiros neste móbil."},
        {"update_badge", "ACTUALIZACIÓN"},
        {"update_title", "Xa está a versión %s"},
        {"update_body", "Tes a versión %s. A actualización instálase enriba: os ficheiros do xogo e as túas partidas quedan como están."},
        {"update_button", "Actualizar"},
        {"retry_update", "Tentar de novo"},
        {"later", "Máis tarde"},
        {"update_note", "Uns %s MB de descarga. Android pídeche que confirmes a instalación."},
        {"permission_title", "Primeiro, un permiso"},
        {"permission_body", "Android necesita o teu permiso unha soa vez para que o xogo instale as súas propias actualizacións. Activa \"Permitir desta fonte\" e volve."},
        {"permission_button", "Abrir o axuste"},
        {"permission_refused", "O permiso segue desactivado, así que a actualización non se pode instalar. Podes activalo e tentalo de novo."},
        {"download_title", "Descargando"},
        {"installing", "Instalando…"},
        {"confirm_install", "Confirma a actualización na pantalla de Android."},
        {"update_failed", "Android non instalou a actualización."},
        {"update_offline", "Non se puido descargar a actualización. Comproba a conexión a internet e téntao de novo."},
    };
}
