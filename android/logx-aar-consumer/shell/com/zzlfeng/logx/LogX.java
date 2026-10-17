package com.zzlfeng.logx;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Random;

public class LogX {
    private static final String ROOT = "/data/local/tmp/logx_aar_smoke";
    private static final byte[] KEY_KID1 = hex("1111111122222222333333334444444455555555666666667777777788888888");

    static {
        System.load(ROOT + "/libc++_shared.so");
        System.load(ROOT + "/liblogx.so");
    }

    public interface KeyProvider {
        byte[] getKey(int kid);
    }

    public interface ExportCallback {
        int onExport(String date, String path, long size);
    }

    public static void onNativeError(int kind, int code, String msg) {
        System.out.println("EVENT kind=" + kind + " code=" + code + " msg=" + msg);
    }

    private static native long nativeInit(String logDir, String stagingPath,
                                          int compressAlgo, int encryptEnable, int kid,
                                          Object keyProvider, int retentionDays, long maxFileSize,
                                          int maxLogLength, int flushThreshold,
                                          boolean consoleEcho, int stagingWaitMs);

    private static native void nativeClose(long handle);

    private static native int nativeWrite(long handle, int level, String tag, byte[] payload);

    private static native int nativeFlush(long handle);

    private static native int nativeExport(long handle, String from, String to, Object callback);

    private static native long[] nativeGetStats(long handle);

    public static void main(String[] args) throws Exception {
        File root = new File(ROOT);
        File logDir = new File(root, "logs");
        File exportDir = new File(root, "export");
        logDir.mkdirs();
        exportDir.mkdirs();

        long h = nativeInit(logDir.getAbsolutePath(), new File(root, "staging.bin").getAbsolutePath(),
                1, 1, 1,
                (KeyProvider) kid -> kid == 1 ? KEY_KID1 : null,
                7, 64 * 1024, 64 * 1024, 4096, false, 1000);
        if (h == 0) {
            System.out.println("CONSUMER_SMOKE_FAIL init=0");
            return;
        }

        int writeFails = 0;
        Random rnd = new Random(20261017L);
        for (int i = 0; i < 20; i++) {
            byte[] msg = String.format("hello-%02d 中文消息 payload", i).getBytes("UTF-8");
            writeFails += write(h, 2, "Main", msg);
        }
        byte[] big = new byte[40 * 1024];
        rnd.nextBytes(big);
        writeFails += write(h, 3, "Big", big);
        for (int i = 0; i < 120; i++) {
            byte[] prefix = String.format("rnd-%03d:", i).getBytes("UTF-8");
            byte[] body = new byte[1000];
            rnd.nextBytes(body);
            byte[] payload = new byte[prefix.length + body.length];
            System.arraycopy(prefix, 0, payload, 0, prefix.length);
            System.arraycopy(body, 0, payload, prefix.length, body.length);
            writeFails += write(h, 2, "Split", payload);
            if (i % 10 == 9) nativeFlush(h);
        }

        int flushRc = nativeFlush(h);
        long[] stats = nativeGetStats(h);
        String today = new SimpleDateFormat("yyyyMMdd", Locale.US).format(new Date());
        final int[] exported = {0};
        int exportRc = nativeExport(h, today, today, (ExportCallback) (date, path, size) -> {
            try {
                copy(new File(path), new File(exportDir, String.format("plain_%s_%02d.txt", date, exported[0])));
                exported[0]++;
                System.out.println("EXPORT date=" + date + " size=" + size);
                return 0;
            } catch (IOException e) {
                e.printStackTrace();
                return 1;
            }
        });

        nativeClose(h);

        StringBuilder files = new StringBuilder();
        File[] logs = logDir.listFiles();
        if (logs != null) {
            for (File f : logs) {
                if (f.isFile()) {
                    files.append(f.getName()).append(':').append(f.length()).append(' ');
                }
            }
        }
        StringBuilder stat = new StringBuilder();
        if (stats != null) {
            for (long v : stats) stat.append(v).append(',');
        }
        System.out.println("CONSUMER_STATS flushRc=" + flushRc + " writeFails=" + writeFails
                + " stats=" + stat);
        System.out.println("CONSUMER_FILES " + files);
        System.out.println("CONSUMER_SMOKE_" + (exportRc == 0 && exported[0] > 0 && writeFails == 0 ? "OK" : "FAIL")
                + " exportRc=" + exportRc + " exportFiles=" + exported[0]);
    }

    private static int write(long h, int level, String tag, byte[] payload) {
        int rc = nativeWrite(h, level, tag, payload);
        if (rc != 0) System.out.println("WRITE_FAIL tag=" + tag + " rc=" + rc);
        return rc == 0 ? 0 : 1;
    }

    private static void copy(File src, File dst) throws IOException {
        try (FileInputStream in = new FileInputStream(src);
             FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static byte[] hex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }
}
