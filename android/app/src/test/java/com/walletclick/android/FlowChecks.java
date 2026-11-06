package com.walletclick.android;

import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Runs as a plain Java program as well as from JUnit. No device or transactions. */
public final class FlowChecks {
    static int checks;
    static void check(boolean ok, String message) { checks++; if(!ok) throw new AssertionError(message); }
    static final class Fake implements Flow.Screen {
        String page="home", failure="";
        List<String> clicks=new ArrayList<>();
        List<BigDecimal> amounts=new ArrayList<>();
        String typed="";
        boolean oldSuccess,locked;
        Fake(String... values) { for(String v:values) amounts.add(new BigDecimal(v)); }
        public void foreground() { if(locked) throw new IllegalStateException("locked"); }
        public boolean has(String role) {
            if(role.equals("error")) return failure.equals("password") && page.equals("password");
            if(role.equals("home") && page.equals("success") && !failure.equals("stuck")) page="home";
            if(role.equals("success")) return oldSuccess || (page.equals("success") && !failure.equals("unknown"));
            return (role.equals("home") && page.equals("home")) || (role.equals("fee_ready") && page.equals("fee")) || (role.equals("password") && page.equals("password"));
        }
        public BigDecimal fee() {
            if(failure.equals("read")) throw new IllegalStateException("fee failed");
            BigDecimal result=amounts.get(0);
            if(failure.equals("changing")) amounts.set(0,result.add(BigDecimal.ONE));
            return result;
        }
        public void click(String role) {
            clicks.add(role);
            switch(role) {
                case "claim": page="fee"; break;
                case "cancel": amounts.remove(0); page="home"; break;
                case "confirm": page="password"; break;
                case "submit": page="success"; break;
                default: throw new AssertionError(role);
            }
        }
        public void password(char[] value) { typed=new String(value); }
    }
    static void drive(Flow flow,Fake fake,int ticks) { for(int i=0;i<ticks && flow.running();i++) flow.tick(i*250L,fake); }
    static Flow start(int limit) { Flow f=new Flow(); f.start(0,1000,limit,2000,"local-test-only".toCharArray()); return f; }
    public static void runChecks() {
        checks=0;
        check(Flow.amount("0.000 ETH").signum()==0,"zero");
        check(Flow.amount("0.000000000000000001 ETH").compareTo(new BigDecimal("0.000000000000000001"))==0,"tiny positive");
        check(Flow.amount("1,234.56 USDT").compareTo(new BigDecimal("1234.56"))==0,"grouping");
        for(String text:new String[]{"", "加载中", "0 loading", "≈0", "<0.01 ETH", "-1", "NaN", "1e-8", "1,23", "0 ETH ($0)", "O.00"}) {
            boolean failed=false; try { Flow.amount(text); } catch(IllegalStateException e) { failed=true; } check(failed,"reject "+text);
        }
        Flow f=start(1); Fake fake=new Fake("0","0.0001"); drive(f,fake,80);
        check(f.state()==Flow.State.DONE,"zero then positive done");
        check(f.attempts()==2 && f.successes()==1,"count successes only");
        check(fake.clicks.equals(Arrays.asList("claim","cancel","claim","confirm","submit")),"exact actions");
        check(fake.typed.equals("local-test-only"),"password input");
        f=start(3); fake=new Fake("1"); drive(f,fake,120);
        check(f.successes()==3 && f.state()==Flow.State.DONE,"finite success count");
        f=start(0); fake=new Fake("1"); drive(f,fake,120);
        check(f.successes()>3 && f.running(),"unlimited"); int before=fake.clicks.size(); f.stop(); drive(f,fake,5);
        check(fake.clicks.size()==before && f.state()==Flow.State.STOPPED,"stop blocks subsequent actions");
        for(String failure:new String[]{"read","changing","unknown","password","stuck"}) {
            f=start(2); fake=new Fake("1"); fake.failure=failure; drive(f,fake,80);
            check(f.state()==Flow.State.PAUSED,"pause "+failure);
            check(java.util.Collections.frequency(fake.clicks,"submit")<=1,"no resend "+failure);
        }
        f=start(1); fake=new Fake("1"); fake.oldSuccess=true; drive(f,fake,5);
        check(fake.clicks.isEmpty() && f.state()==Flow.State.PAUSED,"stale success");
        f=start(1); fake=new Fake("1"); fake.locked=true; drive(f,fake,5);
        check(fake.clicks.isEmpty() && f.state()==Flow.State.PAUSED,"lock pauses");
        f=start(1); fake=new Fake("1");
        f.tick(0,fake); f.tick(250,fake); f.tick(750,fake); f.stop(); f.tick(1000,fake);
        check(fake.typed.isEmpty() && !fake.clicks.contains("submit"),"stop before password");
        Flow a=start(1), b=start(2); Fake af=new Fake("1"),bf=new Fake("2"); af.failure="read";
        for(int i=0;i<100;i++) { a.tick(i*250L,af); b.tick(i*250L,bf); }
        check(a.state()==Flow.State.PAUSED && b.successes()==2,"independent instances");
    }
    public static void main(String[] args) { runChecks(); System.out.println("Flow checks passed: "+checks); }
}
