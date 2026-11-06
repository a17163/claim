package com.walletclick.android;

import android.content.Context;
import android.content.SharedPreferences;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.LinkedHashMap;
import java.util.Map;

final class Config {
    final SharedPreferences prefs;
    String wallet;
    long interval;
    int limit;
    final Map<String, String> rules = new LinkedHashMap<>();
    Config(Context context) {
        prefs = context.getSharedPreferences("config", Context.MODE_PRIVATE);
        wallet = prefs.getString("wallet", ""); interval = prefs.getLong("interval", 1000); limit = prefs.getInt("limit", 1);
        for (String key : Flow.ROLES) rules.put(key, prefs.getString("rule." + key, ""));
    }
    void validate(boolean checks) {
        if (!wallet.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+") || wallet.equals("com.walletclick.android"))
            throw new IllegalStateException("请填写真实目标钱包包名");
        if (interval <= 0 || interval > 86400000L || limit < 0) throw new IllegalStateException("间隔须为 0.001–86400 秒，次数非负");
        for (String key : Flow.ROLES) {
            String value = rules.get(key);
            if (value.isEmpty() && key.equals("error")) continue;
            if (value.isEmpty()) throw new IllegalStateException("未填写：" + key);
            NodeAccess.checkRule(value);
        }
        if (checks) for (int i = 0; i < 4; i++)
            if (!fingerprint().equals(prefs.getString("verified." + i, "")))
                throw new IllegalStateException("请先通过悬浮按钮验证全部四个阶段");
    }
    String fingerprint() {
        try {
            StringBuilder value = new StringBuilder(wallet);
            for (String rule : rules.values()) value.append('\n').append(rule);
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(value.toString().getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder();
            for (byte b : digest) hex.append(String.format("%02x", b));
            return hex.toString();
        } catch (Exception e) { throw new IllegalStateException("配置指纹计算失败"); }
    }
    void save() {
        SharedPreferences.Editor edit = prefs.edit().putString("wallet", wallet).putLong("interval", interval).putInt("limit", limit);
        for (Map.Entry<String, String> entry : rules.entrySet()) edit.putString("rule." + entry.getKey(), entry.getValue());
        if (!edit.commit()) throw new IllegalStateException("配置保存失败");
    }
    void verify(int stage) { prefs.edit().putString("verified." + stage, fingerprint()).apply(); }
    void clearChecks() { SharedPreferences.Editor edit = prefs.edit(); for (int i=0;i<4;i++) edit.remove("verified."+i); edit.apply(); }
}
