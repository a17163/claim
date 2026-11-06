package com.walletclick.android;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.math.BigDecimal;
import java.util.Arrays;
import java.util.List;

public final class WalletService extends AccessibilityService {
    private static java.lang.ref.WeakReference<WalletService> current=new java.lang.ref.WeakReference<>(null);
    static WalletService active() { return current.get(); }
    static final String[] STAGES={"领取页", "费用弹窗", "密码页", "成功提示"};
    final Flow flow=new Flow();
    private final Handler handler=new Handler(Looper.getMainLooper());
    private NodeAccess screen;
    private WindowManager manager;
    private View panel, inspector;
    private TextView status;
    private Spinner stage;
    private int token;
    private boolean verifying;
    private final Runnable tick=new Runnable() {
        @Override public void run() {
            if (flow.running() && screen!=null) { flow.tick(SystemClock.elapsedRealtime(),screen); updateStatus(); }
            if(flow.running()) handler.postDelayed(this,250);
        }
    };
    @Override protected void onServiceConnected() {
        if(flow.running()) flow.pause("服务重新连接，请人工检查后重新开始");
        token++; verifying=false; screen=null;
        current=new java.lang.ref.WeakReference<>(this); manager=(WindowManager)getSystemService(WINDOW_SERVICE);
        // No screen content is read until a user presses Start / Validate / Nodes.
        showPanel(); handler.removeCallbacks(tick);
    }
    @Override public void onAccessibilityEvent(AccessibilityEvent event) { /* Poll only during an explicitly started task. */ }
    @Override public boolean onUnbind(Intent intent) {
        stopTask("无障碍服务已断开，任务停止");
        if(panel!=null) { manager.removeView(panel); panel=null; }
        if(active()==this) current.clear();
        return super.onUnbind(intent);
    }
    @Override public void onInterrupt() { stopTask("无障碍服务被中断，已停止"); }
    @Override public void onDestroy() {
        handler.removeCallbacksAndMessages(null); flow.stop();
        removeInspector(); if(panel!=null) { manager.removeView(panel); panel=null; }
        if(active()==this) current.clear(); super.onDestroy();
    }
    private int dp(int value) { return Math.round(value*getResources().getDisplayMetrics().density); }
    private void safe(Runnable action) {
        try { action.run(); }
        catch (IllegalStateException e) { status.setText(e.getMessage()); }
        catch (Exception e) { status.setText("操作失败，请检查配置、节点及系统权限"); }
    }
    private Button button(String text,Runnable action) {
        Button b=new Button(this); b.setText(text); b.setTextSize(12); b.setMinWidth(0); b.setMinimumWidth(0);
        b.setOnClickListener(v -> safe(action)); return b;
    }
    private WindowManager.LayoutParams params(int width) {
        WindowManager.LayoutParams p=new WindowManager.LayoutParams(width,WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE|WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT);
        p.gravity=Gravity.TOP|Gravity.START; p.x=dp(8); p.y=dp(100); return p;
    }
    void showPanel() {
        if (panel!=null) return;
        LinearLayout layout=new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL); layout.setPadding(dp(8),dp(4),dp(8),dp(4));
        layout.setBackgroundColor(Color.rgb(235,243,250));
        TextView handle=new TextView(this) { @Override public boolean performClick() { super.performClick(); return true; } }; handle.setText("自动领取 · 拖动此处移动"); handle.setTextColor(Color.DKGRAY); handle.setPadding(0,dp(8),0,dp(8)); layout.addView(handle);
        status=new TextView(this); status.setTextSize(12); status.setTextColor(Color.BLACK); layout.addView(status);
        LinearLayout row=new LinearLayout(this);
        row.addView(button("开始",this::startTask)); row.addView(button("停止",() -> stopTask("已停止；已发出的交易无法撤回")));
        row.addView(button("设置",() -> { stopTask("打开设置，任务已停止"); startActivity(new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }));
        layout.addView(row);
        stage=new Spinner(this); stage.setAdapter(new ArrayAdapter<>(this,android.R.layout.simple_spinner_dropdown_item,STAGES)); layout.addView(stage);
        LinearLayout checks=new LinearLayout(this); checks.addView(button("验证",this::validateStage)); checks.addView(button("节点",this::inspect));
        checks.addView(button("隐藏",() -> { stopTask("悬浮控制已隐藏，任务已停止"); manager.removeView(panel); panel=null; })); layout.addView(checks);
        WindowManager.LayoutParams p=params(dp(255));
        handle.setOnTouchListener(new View.OnTouchListener() {
            float downX,downY; int originX,originY;
            public boolean onTouch(View view,MotionEvent e) {
                if(e.getAction()==MotionEvent.ACTION_DOWN) { downX=e.getRawX(); downY=e.getRawY(); originX=p.x; originY=p.y; return true; }
                if(e.getAction()==MotionEvent.ACTION_MOVE) { p.x=Math.max(0,originX+(int)(e.getRawX()-downX)); p.y=Math.max(0,originY+(int)(e.getRawY()-downY)); manager.updateViewLayout(panel,p); return true; }
                if(e.getAction()==MotionEvent.ACTION_UP) { view.performClick(); return true; } return false;
            }
        });
        panel=layout; manager.addView(panel,p); updateStatus();
    }
    private void updateStatus() {
        if(status!=null) status.setText("尝试 "+flow.attempts()+" / 成功 "+flow.successes()+"\n费用："+flow.feeText()+"\n"+flow.message());
    }
    void stopTask(String message) { token++; verifying=false; flow.stop(); handler.removeCallbacks(tick); removeInspector(); if(status!=null) status.setText(message); }
    private void idle() { if(!new Config(this).prefs.getBoolean("consent",false)) throw new IllegalStateException("请先打开 App 阅读并同意无障碍用途说明"); if(flow.running() || verifying) throw new IllegalStateException("请先停止当前任务或等待验证结束"); }
    private void startTask() {
        idle(); Config cfg=new Config(this); cfg.validate(true);
        if (!cfg.prefs.getBoolean("consent",false)) throw new IllegalStateException("请先打开 App 阅读并同意无障碍用途说明");
        screen=new NodeAccess(this,cfg); screen.foreground();
        char[] password=Vault.load(this,cfg.wallet);
        try { flow.start(SystemClock.elapsedRealtime(),cfg.interval,cfg.limit,20000,password); }
        finally { Arrays.fill(password,'\0'); }
        AccessibilityServiceInfo info=getServiceInfo(); info.packageNames=new String[]{cfg.wallet}; setServiceInfo(info);
        updateStatus(); handler.removeCallbacks(tick); handler.post(tick);
    }
    private void validateStage() {
        idle(); Config cfg=new Config(this); cfg.validate(false);
        NodeAccess access=new NodeAccess(this,cfg); access.foreground(); int selected=stage.getSelectedItemPosition();
        if(selected==0) {
            access.requireNode("home"); access.requireClickable("claim");
            if(access.has("fee_ready") || access.has("password") || access.has("success")) throw new IllegalStateException("领取页不应有费用、密码或旧成功弹窗");
        } else if(selected==1) {
            access.requireNode("fee_ready"); access.requireClickable("cancel"); access.requireClickable("confirm");
            BigDecimal first=access.fee(); int current=++token; verifying=true; status.setText("只读核对费用…");
            handler.postDelayed(() -> {
                if(current!=token) return;
                verifying=false;
                safe(() -> {
                    access.foreground(); access.requireNode("fee_ready");
                    if(first.compareTo(access.fee())!=0) throw new IllegalStateException("费用仍在变化，验证失败");
                    cfg.verify(selected); status.setText("费用阶段验证通过："+first.toPlainString());
                });
            },400); return;
        } else if(selected==2) { access.requirePassword(); access.requireClickable("submit"); }
        else access.requireNode("success");
        cfg.verify(selected); status.setText(STAGES[selected]+"验证通过（未点击、未输入）");
    }
    private void removeInspector() { if(inspector!=null) { manager.removeView(inspector); inspector=null; } }
    private void inspect() {
        idle(); Config cfg=new Config(this);
        if(cfg.wallet.isEmpty()) throw new IllegalStateException("先在设置中填写钱包包名并保存");
        List<NodeAccess.Item> items=new NodeAccess(this,cfg).inspect();
        removeInspector(); LinearLayout container=new LinearLayout(this); container.setOrientation(LinearLayout.VERTICAL); container.setBackgroundColor(Color.WHITE); container.setPadding(dp(8),dp(8),dp(8),dp(8));
        TextView heading=new TextView(this); heading.setText("选择用途，再点节点填入候选规则。\n动态金额不应写进定位规则。输入内容已隐藏。"); container.addView(heading);
        Spinner role=new Spinner(this); role.setAdapter(new ArrayAdapter<>(this,android.R.layout.simple_spinner_dropdown_item,Flow.LABELS)); container.addView(role);
        container.addView(button("关闭节点面板",this::removeInspector));
        ScrollView scroll=new ScrollView(this); LinearLayout list=new LinearLayout(this); list.setOrientation(LinearLayout.VERTICAL); scroll.addView(list);
        container.addView(scroll,new LinearLayout.LayoutParams(-1,dp(330)));
        for(NodeAccess.Item item:items) {
            Button node=button(item.description,() -> {
                String key=Flow.ROLES[role.getSelectedItemPosition()];
                cfg.rules.put(key,item.rule); cfg.save();
                status.setText("已填入 "+key+"，请检查规则并重新验证"); removeInspector();
            }); node.setAllCaps(false); node.setGravity(Gravity.START); list.addView(node);
        }
        inspector=container; WindowManager.LayoutParams p=params(dp(320)); p.y=dp(30); manager.addView(inspector,p);
    }
}
