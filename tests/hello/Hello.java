public class Hello {
    public static void main(String[] args) {
        long t0 = System.nanoTime();
        long sum = 0;
        for (int i = 0; i < 1_000_000; i++) sum += i % 7;
        System.out.println("Hello from xiaoji ART (sum=" + sum + ", loop " + (System.nanoTime() - t0) / 1000 + " us)");
    }
}
