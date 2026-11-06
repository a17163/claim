package com.walletclick.android;

import java.math.BigDecimal;
import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Deterministic, clock-driven workflow. No Android dependency, no automatic retry of submissions. */
public final class Flow {
    public interface Screen {
        void foreground();
        boolean has(String role);
        BigDecimal fee();
        void click(String role);
        void password(char[] value);
    }
    public enum State { IDLE, HOME, FEE, STABLE_FEE, PASSWORD, SUBMIT, SUCCESS, RETURN, DELAY, DONE, PAUSED, STOPPED }
    public static final String[] ROLES = {"home", "claim", "fee_ready", "fee", "cancel", "confirm", "password", "submit", "success", "error"};
    public static final String[] LABELS = {"领取页标识", "全部领取", "费用加载完成标识", "费用金额", "关闭费用弹窗", "第一次确认", "密码框", "密码页确认", "提交成功提示", "错误提示（可选）"};
    private static final Pattern AMOUNT = Pattern.compile(
        "(?:[¥$€]\\s*)?((?:0|[1-9]\\d*|[1-9]\\d{0,2}(?:,\\d{3})+)(?:\\.\\d+)?)(?:\\s*(?:ETH|BNB|BTC|SOL|TRX|USDT|USDC|TON|MATIC|POL|AVAX|SUI|APT|OKT|HT|FTM|CRO|DOT|ATOM|ADA|XRP|DOGE|USD|CNY|EUR|GWEI|WEI|gwei|wei|元))?");
    public static BigDecimal amount(String text) {
        Matcher m = AMOUNT.matcher(text == null ? "" : text.trim());
        if (!m.matches()) throw new IllegalStateException("网络费不是明确的单一金额");
        return new BigDecimal(m.group(1).replace(",", ""));
    }
    private State state = State.IDLE;
    private long entered, next, interval, timeout;
    private int limit, attempts, successes;
    private char[] secret;
    private BigDecimal quoted;
    private String lastFee = "—", message = "未开始";
    public State state() { return state; }
    public int attempts() { return attempts; }
    public int successes() { return successes; }
    public String feeText() { return lastFee; }
    public String message() { return message; }
    public boolean running() { return state.ordinal() >= State.HOME.ordinal() && state.ordinal() <= State.DELAY.ordinal(); }
    public void start(long now, long interval, int limit, long timeout, char[] password) {
        if (running()) throw new IllegalStateException("任务已经运行");
        if (interval <= 0 || limit < 0 || timeout <= 0 || password == null || password.length == 0)
            throw new IllegalArgumentException("参数或密码无效");
        this.interval = interval; this.limit = limit; this.timeout = timeout;
        this.secret = password.clone(); attempts = 0; successes = 0; lastFee = "—";
        enter(State.HOME, now, "等待领取页");
    }
    public void stop() { finish(State.STOPPED, "已停止；已发出的交易无法撤回"); }
    public void pause(String reason) { finish(State.PAUSED, reason); }
    private void finish(State target, String text) {
        state = target; message = text;
        if (secret != null) Arrays.fill(secret, '\0');
        secret = null;
    }
    private void enter(State target, long now, String text) { state = target; entered = now; message = text; }
    private void require(boolean ok, String text) { if (!ok) throw new IllegalStateException(text); }
    public void tick(long now, Screen screen) {
        if (!running()) return;
        try {
            screen.foreground();
            require(!screen.has("error"), "钱包显示错误，请人工检查后重新开始");
            if (state != State.DELAY && now - entered > timeout)
                throw new IllegalStateException("等待界面超时；请先人工检查交易结果，不会自动重发");
            switch (state) {
                case HOME:
                    if (!screen.has("home")) return;
                    require(!screen.has("success"), "请先清除上一笔成功提示");
                    require(!screen.has("fee_ready") && !screen.has("password"), "请先关闭钱包弹窗并返回领取页");
                    screen.click("claim"); attempts++;
                    enter(State.FEE, now, "等待费用弹窗"); break;
                case FEE:
                    if (!screen.has("fee_ready")) return;
                    quoted = screen.fee(); lastFee = quoted.toPlainString(); next = now + 400;
                    enter(State.STABLE_FEE, now, "核对网络费"); break;
                case STABLE_FEE:
                    if (now < next) return;
                    require(screen.has("fee_ready"), "费用弹窗已改变");
                    require(quoted.compareTo(screen.fee()) == 0, "网络费仍在变化，请稍后重新开始");
                    if (quoted.signum() == 0) {
                        screen.click("cancel"); enter(State.RETURN, now, "费用为 0，等待返回领取页");
                    } else {
                        require(!screen.has("success"), "旧成功提示仍存在");
                        require(quoted.compareTo(screen.fee()) == 0, "确认前网络费改变");
                        screen.click("confirm"); enter(State.PASSWORD, now, "等待密码框");
                    }
                    break;
                case PASSWORD:
                    if (!screen.has("password")) return;
                    screen.password(secret);
                    enter(State.SUBMIT, now, "准备密码页确认"); break;
                case SUBMIT:
                    require(screen.has("password"), "密码界面已改变");
                    require(!screen.has("success"), "提交前已有成功提示，请检查交易");
                    screen.click("submit"); enter(State.SUCCESS, now, "等待明确提交成功提示"); break;
                case SUCCESS:
                    if (!screen.has("success")) return;
                    successes++;
                    if (limit != 0 && successes >= limit) finish(State.DONE, "已完成；成功次数为钱包提示已提交，非链上最终确认");
                    else enter(State.RETURN, now, "已提交，等待返回领取页");
                    break;
                case RETURN:
                    if (!screen.has("home") || screen.has("success") || screen.has("fee_ready") || screen.has("password")) return;
                    next = now + interval; enter(State.DELAY, now, "等待下一轮"); break;
                case DELAY:
                    if (now >= next) enter(State.HOME, now, "等待领取页");
                    break;
                default: break;
            }
        } catch (IllegalStateException e) { pause(e.getMessage()); }
          catch (Exception e) { pause("操作失败；请人工检查界面与提交结果，不会自动重发"); }
    }
}
