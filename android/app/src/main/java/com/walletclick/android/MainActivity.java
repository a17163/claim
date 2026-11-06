package com.walletclick.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputType;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.math.BigDecimal;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;

public final class MainActivity extends Activity {
    private EditText wallet,interval,limit,password;
    private final Map<String,EditText> fields=new LinkedHashMap<>();
    private TextView status;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved); getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        ScrollView scroll=new ScrollView(this); LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL); body.setPadding(24,32,24,32); scroll.addView(body); setContentView(scroll);
        scroll.setOnApplyWindowInsetsListener((view,insets) -> { body.setPadding(24+insets.getSystemWindowInsetLeft(),32+insets.getSystemWindowInsetTop(),24+insets.getSystemWindowInsetRight(),24+insets.getSystemWindowInsetBottom()); return insets; });
        TextView title=new TextView(this); title.setText("手机自动领取"); title.setTextSize(26); title.setTextColor(Color.rgb(26,54,78)); body.addView(title);
        label(body,"仅在本机运行 · 无需电脑 / Root\n请手动打开钱包和目标网页，从悬浮控制开始。屏幕需保持解锁。费用为 0 重试，大于 0 确认；次数按钱包提示已提交计算。",16);
        status=label(body,"",14);
        wallet=field(body,"目标钱包包名（例如 com.example.wallet）",false);
        interval=field(body,"重试间隔（秒，默认 1）",false); interval.setInputType(InputType.TYPE_CLASS_NUMBER|InputType.TYPE_NUMBER_FLAG_DECIMAL);
        limit=field(body,"成功次数（默认 1，0 表示不限）",false); limit.setInputType(InputType.TYPE_CLASS_NUMBER);
        button(body,"保存配置",this::save);
        label(body,"钱包密码（按钱包包名分别加密保存在本机）",16);
        password=field(body,"输入新密码",true);
        button(body,"保存 / 更新此钱包密码",() -> {
            Config cfg=save(); char[] chars=new char[password.length()]; password.getText().getChars(0,password.length(),chars,0);
            try { Vault.save(this,cfg.wallet,chars); } finally { Arrays.fill(chars,'\0'); password.setText(""); }
            updateStatus(); toast("密码已在本机加密保存");
        });
        button(body,"删除此钱包密码",() -> { Config cfg=save(); Vault.delete(this,cfg.wallet); password.setText(""); updateStatus(); });
        button(body,"开启 / 管理无障碍服务",this::enableService);
        button(body,"显示悬浮控制",() -> {
            if(WalletService.active()==null) throw new IllegalStateException("先在系统设置中启用本 App 的无障碍服务");
            WalletService.active().showPanel(); toast("请手动打开钱包和目标网页");
        });
        button(body,"停止任务",() -> { if(WalletService.active()!=null) WalletService.active().stopTask("已停止；请检查可能已发出的交易"); updateStatus(); });
        label(body,"钱包定位规则",22);
        label(body,"规则：id=完整资源ID、text=精确文字、desc=精确描述、class=控件类名；可用 && 组合。必须唯一匹配可见元素。可在钱包里用悬浮“节点”采集。费用只包含金额及可选单位。",14);
        for(int i=0;i<Flow.ROLES.length;i++) fields.put(Flow.ROLES[i],field(body,Flow.LABELS[i],false));
        button(body,"保存定位规则",this::save);
        button(body,"清除阶段验证",() -> { new Config(this).clearChecks(); updateStatus(); });
        label(body,"适配顺序：领取页 → 费用弹窗 → 密码页 → 成功提示。手动打开对应界面，在悬浮栏选择阶段并验证。验证不会点击或输入。全部通过后返回领取页再开始。\n若钱包不开放费用节点、阻止密码输入或需要验证码/生物识别，会暂停；此版本不含 OCR，不绕过钱包限制。",14);
        load();
    }
    private TextView label(LinearLayout parent,String text,int size) { TextView view=new TextView(this); view.setText(text); view.setTextSize(size); view.setPadding(0,16,0,12); parent.addView(view); return view; }
    private EditText field(LinearLayout parent,String label,boolean secure) {
        label(parent,label,14); EditText view=new EditText(this); view.setSingleLine(true);
        if(secure) { view.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_PASSWORD); view.setSaveEnabled(false); view.setFilterTouchesWhenObscured(true); if(android.os.Build.VERSION.SDK_INT>=34) view.setAccessibilityDataSensitive(android.view.View.ACCESSIBILITY_DATA_SENSITIVE_YES); view.setImportantForAutofill(EditText.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS); }
        else view.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        parent.addView(view); return view;
    }
    private void button(LinearLayout parent,String text,Runnable action) {
        Button b=new Button(this); b.setText(text); b.setOnClickListener(v -> {
            try { action.run(); }
            catch(IllegalStateException e) { toast(e.getMessage()); }
            catch(Exception e) { toast("操作失败，请检查输入和系统权限"); }
        }); parent.addView(b);
    }
    private void toast(String text) { Toast.makeText(this,text,Toast.LENGTH_LONG).show(); }
    private void load() {
        Config cfg=new Config(this); wallet.setText(cfg.wallet); interval.setText(BigDecimal.valueOf(cfg.interval,3).stripTrailingZeros().toPlainString()); limit.setText(String.valueOf(cfg.limit));
        for(String key:Flow.ROLES) fields.get(key).setText(cfg.rules.get(key)); updateStatus();
    }
    private Config save() {
        if(WalletService.active()!=null && WalletService.active().flow.running()) throw new IllegalStateException("请先停止任务再修改配置");
        Config cfg=new Config(this); cfg.wallet=wallet.getText().toString().trim();
        if(!cfg.wallet.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+") || cfg.wallet.equals(getPackageName())) throw new IllegalStateException("请填写真实钱包包名");
        try {
            cfg.interval=new BigDecimal(interval.getText().toString()).movePointRight(3).longValueExact(); cfg.limit=Integer.parseInt(limit.getText().toString());
            if(cfg.interval<=0 || cfg.interval>86400000L || cfg.limit<0) throw new ArithmeticException();
        } catch(Exception e) { throw new IllegalStateException("间隔须为 0.001–86400 秒（最多三位小数），次数为非负整数"); }
        for(String key:Flow.ROLES) { String rule=fields.get(key).getText().toString().trim(); if(!rule.isEmpty()) NodeAccess.checkRule(rule); cfg.rules.put(key,rule); }
        cfg.save(); updateStatus(); return cfg;
    }
    private void updateStatus() {
        Config cfg=new Config(this); int checked=0; for(int i=0;i<4;i++) if(cfg.fingerprint().equals(cfg.prefs.getString("verified."+i,""))) checked++;
        status.setText("服务："+(WalletService.active()==null?"未启用":"已连接")+" · 阶段验证："+checked+"/4\n当前钱包密码："+(Vault.exists(this,cfg.wallet)?"已加密保存":"未设置"));
    }
    private void enableService() {
        new AlertDialog.Builder(this).setTitle("无障碍用途说明")
            .setMessage("启用后，本 App 可读取你指定钱包的界面文字、按钮、网络费和成功提示，并按你设置的规则点击、输入你保存的密码。\n\n只有点击悬浮“开始”后才自动运行；验证和节点检查由你手动触发。不会联网或上传屏幕。密码以 Android Keystore 加密存储。\n\n请确认你愿意授予此能力；可通过悬浮停止按钮或系统设置随时停用。")
            .setNegativeButton("取消",null).setPositiveButton("同意并打开设置",(d,w) -> {
                new Config(this).prefs.edit().putBoolean("consent",true).apply();
                startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS));
            }).show();
    }
    @Override protected void onResume() { super.onResume(); if(status!=null) load(); }
    @Override protected void onPause() { if(password!=null) password.setText(""); super.onPause(); }
}
