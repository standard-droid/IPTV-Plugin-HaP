package com.jopsis.httpaceserveproxy;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.List;

/** User-tunable AceServe engine options, applied as command-line arguments on engine start. */
final class EngineSettings {
    static final int LIVE_CACHE_MB_DEFAULT = 100;
    static final int LIVE_CACHE_MB_MIN = 16;
    static final int LIVE_CACHE_MB_MAX = 1024;

    private static final String KEY_LIVE_CACHE_MB = "engine_live_cache_mb";
    private static final String KEY_UPNP_ENABLED = "engine_upnp_enabled";
    private static final String KEY_DEBUG_LOGGING = "engine_debug_logging";
    private static final String KEY_EXTRA_OPTIONS = "engine_extra_options";

    private EngineSettings() {
    }

    static int liveCacheMb(Context context) {
        return prefs(context).getInt(KEY_LIVE_CACHE_MB, LIVE_CACHE_MB_DEFAULT);
    }

    static boolean isUpnpEnabled(Context context) {
        return prefs(context).getBoolean(KEY_UPNP_ENABLED, false);
    }

    static boolean isDebugLoggingEnabled(Context context) {
        return prefs(context).getBoolean(KEY_DEBUG_LOGGING, false);
    }

    static String extraOptions(Context context) {
        String value = prefs(context).getString(KEY_EXTRA_OPTIONS, "");
        return value == null ? "" : value;
    }

    static void save(Context context, int liveCacheMb, boolean upnpEnabled, boolean debugLogging, String extraOptions) {
        prefs(context).edit()
                .putInt(KEY_LIVE_CACHE_MB, liveCacheMb)
                .putBoolean(KEY_UPNP_ENABLED, upnpEnabled)
                .putBoolean(KEY_DEBUG_LOGGING, debugLogging)
                .putString(KEY_EXTRA_OPTIONS, extraOptions == null ? "" : extraOptions.trim())
                .commit();
    }

    /** Engine arguments for the current settings, in the order they go after HaP's fixed ones. */
    static List<String> commandArguments(Context context) {
        List<String> args = new ArrayList<>();
        args.add("--live-mem-cache-size");
        args.add(String.valueOf((long) liveCacheMb(context) * 1024 * 1024));
        if (!isUpnpEnabled(context)) args.add("--disable-upnp");
        if (isDebugLoggingEnabled(context)) {
            args.add("--log-debug");
            args.add("355");
            args.add("--log-modules");
            args.add("root:D");
        }
        args.addAll(parseExtraOptions(extraOptions(context)));
        return args;
    }

    /** One option or value per line, like acestream.conf; blank lines and # comments are skipped. */
    static List<String> parseExtraOptions(String text) {
        List<String> args = new ArrayList<>();
        if (text == null) return args;
        for (String line : text.split("\\r?\\n")) {
            String token = line.trim();
            if (token.isEmpty() || token.startsWith("#")) continue;
            args.add(token);
        }
        return args;
    }

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PlaylistCatalog.PREFS_NAME, Context.MODE_PRIVATE);
    }
}
