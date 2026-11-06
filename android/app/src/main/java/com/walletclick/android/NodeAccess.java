package com.walletclick.android;

import android.accessibilityservice.AccessibilityService;
import android.app.KeyguardManager;
import android.content.Context;
import android.os.Bundle;
import android.os.PowerManager;
import android.view.accessibility.AccessibilityNodeInfo;
import java.math.BigDecimal;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

final class NodeAccess implements Flow.Screen {
    final AccessibilityService service;
    final Config config;
    NodeAccess(AccessibilityService service, Config config) { this.service = service; this.config = config; }
    static void checkRule(String rule) {
        for (String part : rule.split("&&", -1)) {
            int at = part.indexOf('=');
            if (at <= 0 || part.substring(at + 1).trim().isEmpty() ||
                !java.util.Arrays.asList("id", "text", "desc", "class").contains(part.substring(0, at).trim()))
                throw new IllegalStateException("规则须为 id=…、text=…、desc=…、class=…，可用 && 组合");
        }
    }
    private AccessibilityNodeInfo root() {
        PowerManager power = (PowerManager) service.getSystemService(Context.POWER_SERVICE);
        KeyguardManager lock = (KeyguardManager) service.getSystemService(Context.KEYGUARD_SERVICE);
        if (!power.isInteractive() || lock.isKeyguardLocked()) throw new IllegalStateException("手机已锁屏或熄屏，任务暂停");
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) throw new IllegalStateException("无法读取当前窗口，钱包可能不开放无障碍节点");
        if (!config.wallet.contentEquals(root.getPackageName() == null ? "" : root.getPackageName())) {
            root.recycle(); throw new IllegalStateException("目标钱包不在前台，请手动打开目标网页");
        }
        return root;
    }
    @Override public void foreground() { AccessibilityNodeInfo node = root(); node.recycle(); }
    private boolean matches(AccessibilityNodeInfo node, String rule) {
        for (String part : rule.split("&&")) {
            int at = part.indexOf('='); String key = part.substring(0, at).trim(), value = part.substring(at+1).trim();
            CharSequence actual;
            switch (key) {
                case "id": actual = node.getViewIdResourceName(); break;
                case "text": actual = node.isPassword() ? null : node.getText(); break;
                case "desc": actual = node.isPassword() ? null : node.getContentDescription(); break;
                case "class": actual = node.getClassName(); break;
                default: return false;
            }
            if (actual == null || !value.contentEquals(actual)) return false;
        }
        return true;
    }
    private AccessibilityNodeInfo find(String role, boolean required) {
        String rule = config.rules.get(role);
        if (rule == null || rule.isEmpty()) {
            if (!required) return null;
            throw new IllegalStateException("缺少定位规则：" + role);
        }
        checkRule(rule);
        ArrayDeque<AccessibilityNodeInfo> queue = new ArrayDeque<>(); queue.add(root());
        AccessibilityNodeInfo found = null;
        int count = 0;
        try {
            while (!queue.isEmpty()) {
                AccessibilityNodeInfo node = queue.remove();
                try {
                    if (++count > 4000) throw new IllegalStateException("界面节点过多，请重新适配");
                    for (int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo child = node.getChild(i); if (child != null) queue.add(child); }
                    if (node.isVisibleToUser() && matches(node, rule)) {
                        if (found != null) throw new IllegalStateException("定位匹配多个可见元素：" + role);
                        found = AccessibilityNodeInfo.obtain(node);
                    }
                } finally { node.recycle(); }
            }
            if (found == null && required) throw new IllegalStateException("未找到目标元素：" + role);
            AccessibilityNodeInfo result = found; found = null; return result;
        } finally {
            if (found != null) found.recycle();
            while (!queue.isEmpty()) queue.remove().recycle();
        }
    }
    @Override public boolean has(String role) { AccessibilityNodeInfo node = find(role, false); if (node == null) return false; node.recycle(); return true; }
    void requireNode(String role) { AccessibilityNodeInfo node = find(role, true); node.recycle(); }
    private AccessibilityNodeInfo clickable(String role) {
        AccessibilityNodeInfo node = find(role, true);
        for (int i=0;i<6;i++) {
            if (node.isClickable()) {
                if (node.isEnabled() && node.isVisibleToUser()) return node;
                node.recycle(); throw new IllegalStateException("目标按钮当前不可用：" + role);
            }
            AccessibilityNodeInfo parent = node.getParent(); node.recycle(); node = parent;
            if (node == null) break;
        }
        if (node != null) node.recycle();
        throw new IllegalStateException("目标元素无可点击按钮：" + role);
    }
    void requireClickable(String role) { AccessibilityNodeInfo node = clickable(role); node.recycle(); }
    @Override public void click(String role) {
        AccessibilityNodeInfo node = clickable(role);
        try { foreground(); if (!node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) throw new IllegalStateException("点击未被钱包接受：" + role); }
        finally { node.recycle(); }
    }
    @Override public BigDecimal fee() {
        AccessibilityNodeInfo node = find("fee", true);
        try {
            if (node.isPassword()) throw new IllegalStateException("费用规则误匹配密码字段");
            CharSequence text = node.getText(); if (text == null || text.length() == 0) text = node.getContentDescription();
            return Flow.amount(text == null ? "" : text.toString());
        } finally { node.recycle(); }
    }
    void requirePassword() {
        AccessibilityNodeInfo node = find("password", true);
        try {
            if (!node.isEditable() || !node.isEnabled() ||
                !node.getActionList().contains(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_TEXT))
                throw new IllegalStateException("密码框不支持无障碍输入，请单独适配");
        } finally { node.recycle(); }
    }
    @Override public void password(char[] value) {
        requirePassword();
        AccessibilityNodeInfo node = find("password", true);
        try {
            foreground(); Bundle args = new Bundle();
            args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, new String(value));
            if (!node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) throw new IllegalStateException("钱包拒绝密码输入");
            args.clear();
        } finally { node.recycle(); }
    }
    static final class Item {
        final String description, rule;
        Item(String description, String rule) { this.description = description; this.rule = rule; }
    }
    List<Item> inspect() {
        List<Item> result = new ArrayList<>();
        ArrayDeque<AccessibilityNodeInfo> queue = new ArrayDeque<>(); queue.add(root()); int count=0;
        try {
            while (!queue.isEmpty()) {
                AccessibilityNodeInfo node = queue.remove();
                try {
                    if (++count > 4000) throw new IllegalStateException("界面节点过多");
                    for (int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo child=node.getChild(i); if (child!=null) queue.add(child); }
                    if (!node.isVisibleToUser()) continue;
                    String id=node.getViewIdResourceName(), cls=String.valueOf(node.getClassName());
                    String text=node.isPassword() || node.isEditable() ? "[输入内容隐藏]" : String.valueOf(node.getText());
                    String desc=node.isPassword() || node.isEditable() ? "" : String.valueOf(node.getContentDescription());
                    String rule=id != null ? "id="+id : "class="+cls;
                    if (id==null && !node.isPassword() && !node.isEditable()) {
                        if (node.getText()!=null && node.getText().length()>0) rule += " && text="+node.getText();
                        else if (node.getContentDescription()!=null && node.getContentDescription().length()>0) rule += " && desc="+node.getContentDescription();
                    }
                    result.add(new Item(cls+"\nID: "+id+"\n文字: "+text+"\n描述: "+desc, rule));
                } finally { node.recycle(); }
            }
        } finally { while (!queue.isEmpty()) queue.remove().recycle(); }
        return result;
    }
}
