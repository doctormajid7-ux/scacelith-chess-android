package com.scacelith.game;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.HttpURLConnection;
import java.net.NoRouteToHostException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.security.MessageDigest;
import java.security.cert.CertPathValidatorException;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/**
 * The HTTPS transport of the Android build (src/net/transport_android.cpp): one exchange over the
 * system's own TLS stack (HttpURLConnection), its body handed to the native side piece by piece.
 *
 * Same contract as the desktop transports (src/net/transport.h): redirects are never followed,
 * no cookies, the body arrives as sent (no transparent gzip: the downloads hash the bytes), and a
 * pinned certificate (SHA-256 of the leaf, DER) replaces the system trust store for that host.
 *
 * Called from native worker threads; abort() may come from any other thread (a cancelled
 * download) and closes the connection under the blocked read.
 */
final class Http {
    private volatile HttpURLConnection conn;
    private volatile boolean aborted;
    /** [0] error ("" = the exchange completed), [1] detail for the log. */
    final String[] result = {"", ""};

    void abort() {
        aborted = true;
        HttpURLConnection c = conn;
        if (c != null) c.disconnect();
    }

    /**
     * Runs the exchange. headers: name, value pairs. body: null for none. The head goes to
     * Native.nativeHttpHead(ctx, ...), each piece of the body to Native.nativeHttpBody(ctx, ...);
     * either returning false stops the exchange ("" when the head stopped it, "aborted" when the
     * body did). Returns nothing: see result.
     */
    void run(String url, String method, String[] headers, byte[] body, int timeoutMs, String pinHex, long ctx) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            conn = c;
            if (aborted) { fail("cancelled", "aborted before connecting"); return; }
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setRequestMethod(method);
            c.setRequestProperty("User-Agent", "Scacelith");
            c.setRequestProperty("Accept-Encoding", "identity");
            for (int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
            if (c instanceof HttpsURLConnection && pinHex != null && !pinHex.isEmpty()) pin((HttpsURLConnection) c, pinHex);
            if (body != null) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream out = c.getOutputStream()) { out.write(body); }
            }
            int status = c.getResponseCode();
            List<String> pairs = new ArrayList<>();
            for (Map.Entry<String, List<String>> e : c.getHeaderFields().entrySet()) {
                if (e.getKey() == null) continue;   // the status line
                for (String v : e.getValue()) {
                    pairs.add(e.getKey().toLowerCase(Locale.ROOT));
                    pairs.add(v);
                }
            }
            if (!Native.nativeHttpHead(ctx, status, pairs.toArray(new String[0]))) return;
            long expected = c.getContentLengthLong();
            long got = 0;
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                try (InputStream s = in) {
                    byte[] buf = new byte[64 * 1024];
                    for (int n; (n = s.read(buf)) > 0; ) {
                        got += n;
                        if (!Native.nativeHttpBody(ctx, buf, n)) { fail("aborted", "the receiver stopped the body"); return; }
                    }
                }
            }
            if (expected >= 0 && got < expected) fail("truncated", got + " of " + expected + " bytes");
        } catch (SocketTimeoutException e) {
            fail(aborted ? "cancelled" : "timeout", String.valueOf(e.getMessage()));
        } catch (SSLHandshakeException e) {
            fail(aborted ? "cancelled" : (isCertificate(e) ? "certificate" : "tls"), String.valueOf(e.getMessage()));
        } catch (SSLException e) {
            fail(aborted ? "cancelled" : "tls", String.valueOf(e.getMessage()));
        } catch (UnknownHostException | ConnectException | NoRouteToHostException e) {
            fail(aborted ? "cancelled" : "network", String.valueOf(e.getMessage()));
        } catch (IOException | RuntimeException e) {
            fail(aborted ? "cancelled" : "network", e.getClass().getSimpleName() + ": " + e.getMessage());
        } finally {
            HttpURLConnection c = conn;
            conn = null;
            if (c != null) c.disconnect();
        }
    }

    private void fail(String error, String detail) {
        result[0] = error;
        result[1] = detail;
    }

    private static boolean isCertificate(Throwable e) {
        for (Throwable t = e; t != null; t = t.getCause())
            if (t instanceof CertPathValidatorException || t instanceof CertificateException) return true;
        return false;
    }

    /** Trust exactly the leaf whose DER hashes to pinHex (the desktop transports' pinning). */
    private static void pin(HttpsURLConnection c, String pinHex) throws IOException {
        final String want = pinHex.toLowerCase(Locale.ROOT);
        TrustManager tm = new X509TrustManager() {
            @Override public void checkClientTrusted(X509Certificate[] chain, String auth) throws CertificateException {
                throw new CertificateException("no client certificates");
            }
            @Override public void checkServerTrusted(X509Certificate[] chain, String auth) throws CertificateException {
                if (chain == null || chain.length == 0) throw new CertificateException("no certificate");
                String got;
                try {
                    got = hex(MessageDigest.getInstance("SHA-256").digest(chain[0].getEncoded()));
                } catch (Exception e) {
                    throw new CertificateException(e);
                }
                if (!MessageDigest.isEqual(got.getBytes(), want.getBytes()))
                    throw new CertificateException("pinned certificate mismatch");
            }
            @Override public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
        };
        try {
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, new TrustManager[] {tm}, null);
            c.setSSLSocketFactory(ctx.getSocketFactory());
            // The pin names the one certificate this host may present: it replaces the name check
            // too (a pinned development server is reached by IP or a local name).
            c.setHostnameVerifier((host, session) -> true);
        } catch (Exception e) {
            throw new IOException("cannot set up the pinned TLS context", e);
        }
    }

    private static String hex(byte[] b) {
        StringBuilder s = new StringBuilder(b.length * 2);
        for (byte x : b) s.append(String.format(Locale.ROOT, "%02x", x));
        return s.toString();
    }
}
