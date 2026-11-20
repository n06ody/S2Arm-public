// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

/** Production preferences; the library passes a snapshot across the :game boundary. */
final class AndroidLdnSettings {
    private static final String PREFS = "ldn";
    private static final String MODE = "org.s2arm.android.LDN_MODE";
    private static final String ADDRESS = "org.s2arm.android.LDN_ADDRESS";
    private static boolean applied;

    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private static LdnSettings load(Context context) {
        SharedPreferences prefs = preferences(context);
        return new LdnSettings(prefs.getString("mode", "auto"), prefs.getString("address", ""));
    }

    static void copyToLaunch(Context context, Intent intent) {
        LdnSettings settings = load(context);
        intent.putExtra(MODE, settings.mode);
        intent.putExtra(ADDRESS, settings.address);
    }

    /**
     * Once per process, before createGame/create initializes Host's static LDN policy.
     * Supported game stop ends :game via bridge.cpp -> s2a_host_shutdown -> _exit;
     * LibraryActivity waits for that PID to disappear before launching a replacement.
     * Activity resume/recreation must not rewrite the environment under an existing Host.
     */
    static synchronized void apply(Context context, Intent intent) {
        if (applied) return;
        LdnSettings settings = intent != null && intent.hasExtra(MODE)
            ? new LdnSettings(intent.getStringExtra(MODE), intent.getStringExtra(ADDRESS)) : load(context);
        settings.apply(new LdnSettings.Environment() {
            public String get(String key) { return Os.getenv(key); }
            public void set(String key, String value) {
                try { Os.setenv(key, value, true); }
                catch (ErrnoException error) { throw new IllegalStateException("Cannot set " + key, error); }
            }
        });
        applied = true;
        if ("client".equals(settings.mode) && settings.address.isEmpty())
            Log.w("S2Arm", "Manual LDN client has no IPv4 address; using 0.0.0.0 (no automatic discovery)");
    }

    static void show(Activity activity) {
        LdnSettings saved = load(activity);
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(16 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad, pad, pad);
        TextView help = new TextView(activity);
        help.setText(R.string.ldn_help);
        content.addView(help);
        RadioGroup modes = new RadioGroup(activity);
        String[] values = {"auto", "host", "client"};
        int[] labels = {R.string.ldn_auto, R.string.ldn_host, R.string.ldn_client};
        RadioButton[] buttons = new RadioButton[values.length];
        for (int i = 0; i < values.length; i++) {
            RadioButton button = new RadioButton(activity);
            buttons[i] = button;
            button.setId(View.generateViewId());
            button.setText(labels[i]);
            modes.addView(button);
            if (values[i].equals(saved.mode)) modes.check(button.getId());
        }
        RadioButton client = buttons[2];
        content.addView(modes);
        EditText address = new EditText(activity);
        address.setSingleLine(true);
        address.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
        address.setHint(R.string.ldn_address);
        address.setText(saved.address);
        content.addView(address);
        android.widget.ScrollView scroll = new android.widget.ScrollView(activity);
        scroll.addView(content);
        TextView result = new TextView(activity);
        content.addView(result);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle(R.string.ldn_settings).setView(scroll)
            .setPositiveButton(R.string.ldn_save, null)
            .setNeutralButton(R.string.ldn_test, null)
            .setNegativeButton(android.R.string.cancel, null).create();
        dialog.setOnShowListener(ignored -> {
            // Override the default dismiss-on-click: input errors stay visible in this dialog.
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                LdnSettings settings = readInput(activity, modes, values, buttons, address, result);
                if (settings == null) return;
                preferences(activity).edit().putString("mode", settings.mode)
                    .putString("address", settings.address).apply();
                Toast.makeText(activity, R.string.ldn_saved, Toast.LENGTH_LONG).show();
                dialog.dismiss();
            });
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> {
                LdnSettings settings = readInput(activity, modes, values, buttons, address, result);
                if (settings == null) return;
                if (!"client".equals(settings.mode)) {
                    result.setText(R.string.ldn_test_client);
                    return;
                }
                android.widget.Button test = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
                test.setEnabled(false);
                result.setText(activity.getString(R.string.ldn_testing, settings.address));
                new Thread(() -> {
                    String message;
                    try {
                        settings.testConnection((target, port, timeout) -> {
                            try (java.net.Socket socket = new java.net.Socket()) {
                                socket.connect(new java.net.InetSocketAddress(target, port), timeout);
                            }
                        });
                        message = activity.getString(R.string.ldn_test_ok, settings.address);
                    } catch (java.io.IOException | SecurityException error) {
                        Log.i("S2Arm", "LDN TCP connection test failed", error);
                        message = activity.getString(R.string.ldn_test_failed, settings.address);
                    }
                    String completed = message;
                    activity.runOnUiThread(() -> {
                        if (activity.isFinishing() || activity.isDestroyed() || !dialog.isShowing()) return;
                        test.setEnabled(true);
                        // Do not label an edited address with a previous target's result.
                        if (modes.getCheckedRadioButtonId() == client.getId()
                                && settings.address.equals(address.getText().toString().trim()))
                            result.setText(completed);
                        else result.setText("");
                    });
                }, "s2a-ldn-test").start();
            });
            address.addTextChangedListener(new android.text.TextWatcher() {
                public void beforeTextChanged(CharSequence text, int start, int count, int after) { }
                public void onTextChanged(CharSequence text, int start, int before, int count) {
                    address.setError(null);
                    result.setText("");
                }
                public void afterTextChanged(android.text.Editable text) { }
            });
            modes.setOnCheckedChangeListener((group, checkedId) -> result.setText(""));
        });
        dialog.show();
    }

    private static int selectedMode(RadioGroup modes, RadioButton[] buttons) {
        int checked = modes.getCheckedRadioButtonId();
        for (int i = 0; i < buttons.length; i++)
            if (buttons[i].getId() == checked) return i;
        return -1;
    }

    private static LdnSettings readInput(Activity activity, RadioGroup modes, String[] values,
            RadioButton[] buttons, EditText address, TextView result) {
        int selected = selectedMode(modes, buttons);
        if (selected < 0 || selected >= values.length) {
            result.setText(R.string.ldn_select_mode);
            return null;
        }
        LdnSettings settings = new LdnSettings(values[selected], address.getText().toString());
        LdnSettings.AddressError error = settings.addressError();
        if (error != null) {
            address.setError(error == LdnSettings.AddressError.EMPTY
                ? activity.getString(R.string.ldn_address_empty)
                : activity.getString(R.string.ldn_address_invalid, settings.address));
            address.requestFocus();
            return null;
        }
        return settings;
    }
}
