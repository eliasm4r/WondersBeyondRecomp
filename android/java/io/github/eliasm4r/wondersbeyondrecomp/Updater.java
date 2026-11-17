package io.github.eliasm4r.wondersbeyondrecomp;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

final class Updater {
    static final String RELEASES = "https://api.github.com/repos/eliasm4r/WondersBeyondRecomp/releases/latest";
    private static final String PREFERENCES = "updates";
    private static final String ACTION = "io.github.eliasm4r.wondersbeyondrecomp.INSTALLED";
    private static final String TOKEN = "token";
    private static final long CHECK_SPACING = 6L * 60 * 60 * 1000;

    static final class Release {
        String tag;
        String url;
        long size;
    }

    interface Progress {
        boolean step(long done, long total);
    }

    interface Outcome {
        void finished(boolean installing, String problem);
    }

    private Updater() {
    }

    static String installedVersion(Context context) {
        try {
            String name = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
            return name != null ? name : "0";
        } catch (PackageManager.NameNotFoundException error) {
            return "0";
        }
    }

    static int[] numbers(String text) {
        int[] result = new int[3];
        int part = 0;
        int i = text.startsWith("v") || text.startsWith("V") ? 1 : 0;
        boolean seen = false;
        for (; i < text.length() && part < 3; i++) {
            char letter = text.charAt(i);
            if (letter >= '0' && letter <= '9') {
                result[part] = result[part] * 10 + (letter - '0');
                seen = true;
            } else if (letter == '.' && seen) {
                part++;
                seen = false;
            } else {
                break;
            }
        }
        return result;
    }

    static boolean newer(String theirs, String mine) {
        int[] a = numbers(theirs);
        int[] b = numbers(mine);
        for (int i = 0; i < 3; i++) {
            if (a[i] != b[i]) {
                return a[i] > b[i];
            }
        }
        return false;
    }

    static Release pending(Context context) {
        SharedPreferences store = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        Release release = new Release();
        release.tag = store.getString("tag", null);
        release.url = store.getString("url", null);
        release.size = store.getLong("size", -1);
        if (release.tag == null || release.url == null || !newer(release.tag, installedVersion(context))) {
            return null;
        }
        return release;
    }

    static void checkInBackground(final Context context) {
        final Context application = context.getApplicationContext();
        final SharedPreferences store = application.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        long now = System.currentTimeMillis();
        if (now - store.getLong("checked", 0) < CHECK_SPACING) {
            return;
        }
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    Release release = latest();
                    SharedPreferences.Editor edit = store.edit().putLong("checked", System.currentTimeMillis());
                    if (release != null) {
                        edit.putString("tag", release.tag).putString("url", release.url).putLong("size", release.size);
                    } else {
                        edit.remove("tag").remove("url").remove("size");
                    }
                    edit.apply();
                } catch (Exception error) {
                }
            }
        }, "update check");
        thread.setDaemon(true);
        thread.setPriority(Thread.MIN_PRIORITY);
        thread.start();
    }

    private static HttpURLConnection open(String address) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(10000);
        connection.setReadTimeout(20000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "WondersBeyondRecomp-Android");
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        return connection;
    }

    private static Release latest() throws Exception {
        HttpURLConnection connection = open(RELEASES);
        try {
            if (connection.getResponseCode() != 200) {
                return null;
            }
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream input = connection.getInputStream()) {
                byte[] buffer = new byte[16384];
                int count;
                while ((count = input.read(buffer)) > 0) {
                    body.write(buffer, 0, count);
                }
            }
            JSONObject json = new JSONObject(body.toString("UTF-8"));
            JSONArray assets = json.optJSONArray("assets");
            if (assets == null) {
                return null;
            }
            for (int i = 0; i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                if (asset.optString("name").toLowerCase(java.util.Locale.ROOT).endsWith(".apk")) {
                    Release release = new Release();
                    release.tag = json.optString("tag_name");
                    release.url = asset.optString("browser_download_url");
                    release.size = asset.optLong("size", -1);
                    return release.tag.isEmpty() || release.url.isEmpty() ? null : release;
                }
            }
            return null;
        } finally {
            connection.disconnect();
        }
    }

    static boolean download(Release release, File target, Progress progress) throws Exception {
        HttpURLConnection connection = open(release.url);
        connection.setRequestProperty("Accept", "application/octet-stream");
        try {
            if (connection.getResponseCode() != 200) {
                throw new Exception("HTTP " + connection.getResponseCode());
            }
            long total = connection.getContentLengthLong() > 0 ? connection.getContentLengthLong() : release.size;
            long done = 0;
            try (InputStream input = connection.getInputStream(); OutputStream output = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                int count;
                while ((count = input.read(buffer)) > 0) {
                    output.write(buffer, 0, count);
                    done += count;
                    if (!progress.step(done, total)) {
                        return false;
                    }
                }
            }
            return true;
        } finally {
            connection.disconnect();
        }
    }

    static void install(final Context context, File apk, final Outcome outcome) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(context.getPackageName());
        if (Build.VERSION.SDK_INT >= 31) {
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        }
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream input = new FileInputStream(apk); OutputStream output = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buffer = new byte[1 << 16];
                int count;
                while ((count = input.read(buffer)) > 0) {
                    output.write(buffer, 0, count);
                }
                session.fsync(output);
            }
            final String token = java.util.UUID.randomUUID().toString();
            BroadcastReceiver receiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context received, Intent intent) {
                    if (!token.equals(intent.getStringExtra(TOKEN))) {
                        return;
                    }
                    int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                    if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                        Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                        if (confirm != null) {
                            confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                            received.startActivity(confirm);
                        }
                        outcome.finished(true, null);
                        return;
                    }
                    try {
                        received.unregisterReceiver(this);
                    } catch (RuntimeException error) {
                    }
                    if (status == PackageInstaller.STATUS_SUCCESS) {
                        outcome.finished(true, null);
                    } else {
                        String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                        outcome.finished(false, status == PackageInstaller.STATUS_FAILURE_ABORTED ? null : message == null ? "" : message);
                    }
                }
            };
            IntentFilter filter = new IntentFilter(ACTION);
            if (Build.VERSION.SDK_INT >= 33) {
                context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                context.registerReceiver(receiver, filter);
            }
            Intent callback = new Intent(ACTION).setPackage(context.getPackageName()).putExtra(TOKEN, token);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent sender = PendingIntent.getBroadcast(context, id, callback, flags);
            session.commit(sender.getIntentSender());
        }
    }
}
