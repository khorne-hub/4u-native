package com.mh4u.port;

import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicInteger;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/**
 * A WebSocket over TLS to a chat relay (the native side: app_native/chat/chat_ws_android.cpp, through static methods and
 * a handle). The TLS is the system's: the relay's certificate is checked against the system's store and its name
 * against the address (as a browser does). One thread per socket connects and reads; text messages are queued for
 * the native side, which polls them. Text goes both ways as UTF-8 bytes. The same class is in MHGU and MH4U for Android.
 */
public final class ChatSocket {
    private static final Map<Integer, ChatSocket> ALL = new ConcurrentHashMap<>();
    private static final AtomicInteger NEXT = new AtomicInteger(1);
    private static final String GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    private static final int MAX_MESSAGE = 1 << 20;

    private final ConcurrentLinkedQueue<byte[]> in = new ConcurrentLinkedQueue<>();
    private final Object writing = new Object();
    private final SecureRandom random = new SecureRandom();
    private volatile int state;  // 0 connecting, 1 open, 2 closed
    private volatile String error = "";
    private volatile Socket socket;
    private OutputStream out;

    public static int open(String url) {
        int id = NEXT.getAndIncrement();
        ChatSocket s = new ChatSocket();
        ALL.put(id, s);
        Thread t = new Thread(() -> s.run(url), "chat-relay");
        t.setDaemon(true);
        t.start();
        return id;
    }

    public static int state(int id) {
        ChatSocket s = ALL.get(id);
        return s == null ? 2 : s.state;
    }

    public static boolean send(int id, byte[] text) {
        ChatSocket s = ALL.get(id);
        if (s == null || s.state != 1) return false;
        try {
            s.frame(0x1, text);
            return true;
        } catch (IOException e) {
            s.fail(e);
            return false;
        }
    }

    public static byte[] poll(int id) {
        ChatSocket s = ALL.get(id);
        return s == null ? null : s.in.poll();
    }

    public static String error(int id) {
        ChatSocket s = ALL.get(id);
        return s == null ? "" : s.error;
    }

    public static void close(int id) {
        ChatSocket s = ALL.remove(id);
        if (s == null) return;
        s.state = 2;
        Socket k = s.socket;
        if (k != null) new Thread(() -> {  // (a goodbye, then the socket closed: off the caller's thread)
            try {
                s.frame(0x8, new byte[] {0x03, (byte) 0xE8});
            } catch (IOException ignored) {
            }
            try {
                k.close();
            } catch (IOException ignored) {
            }
        }, "chat-close").start();
    }

    private void fail(Exception e) {
        if (error.isEmpty()) error = e.getClass().getSimpleName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
        state = 2;
        Socket k = socket;
        if (k != null) try {
            k.close();
        } catch (IOException ignored) {
        }
    }

    private void run(String url) {
        try {
            URI u = new URI(url);
            if (!"wss".equalsIgnoreCase(u.getScheme()) || u.getHost() == null) throw new IOException("not a wss:// address");
            String host = u.getHost();
            int port = u.getPort() > 0 ? u.getPort() : 443;
            String path = u.getRawPath() == null || u.getRawPath().isEmpty() ? "/" : u.getRawPath();
            if (u.getRawQuery() != null) path += "?" + u.getRawQuery();
            Socket raw = new Socket();
            socket = raw;
            raw.connect(new InetSocketAddress(host, port), 10000);
            SSLSocket tls = (SSLSocket) ((SSLSocketFactory) SSLSocketFactory.getDefault()).createSocket(raw, host, port, true);
            socket = tls;
            SSLParameters p = tls.getSSLParameters();
            p.setEndpointIdentificationAlgorithm("HTTPS");  // (the certificate must name this host)
            tls.setSSLParameters(p);
            tls.setSoTimeout(15000);
            tls.startHandshake();
            if (!HttpsURLConnection.getDefaultHostnameVerifier().verify(host, tls.getSession()))
                throw new IOException("the relay's certificate is not for " + host);
            if (state == 2) throw new IOException("closed");
            out = tls.getOutputStream();
            InputStream is = tls.getInputStream();
            byte[] nonce = new byte[16];
            random.nextBytes(nonce);
            String key = Base64.encodeToString(nonce, Base64.NO_WRAP);
            String hello = "GET " + path + " HTTP/1.1\r\nHost: " + host + (port != 443 ? ":" + port : "") +
                           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                           "\r\nSec-WebSocket-Version: 13\r\n\r\n";
            synchronized (writing) {
                out.write(hello.getBytes(StandardCharsets.US_ASCII));
                out.flush();
            }
            String head = readHead(is);
            if (!head.startsWith("HTTP/1.1 101")) throw new IOException(head.split("\r\n", 2)[0]);
            String accept = Base64.encodeToString(
                MessageDigest.getInstance("SHA-1").digest((key + GUID).getBytes(StandardCharsets.US_ASCII)), Base64.NO_WRAP);
            if (!head.toLowerCase(java.util.Locale.ROOT).contains("sec-websocket-accept: " + accept.toLowerCase(java.util.Locale.ROOT)))
                throw new IOException("the relay did not accept the WebSocket");
            tls.setSoTimeout(0);  // (a relay may stay quiet a long time)
            if (state == 2) throw new IOException("closed");
            state = 1;
            read(is);
        } catch (Exception e) {
            fail(e);
        }
    }

    private static String readHead(InputStream is) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        int c, n = 0;
        while ((c = is.read()) >= 0) {
            b.write(c);
            n = (c == '\r' || c == '\n') ? n + 1 : 0;
            if (n == 4) break;
            if (b.size() > 16384) throw new IOException("the relay's answer is too long");
        }
        if (c < 0) throw new IOException("the relay closed the connection");
        return b.toString("ISO-8859-1");
    }

    private static void readFully(InputStream is, byte[] b, int len) throws IOException {
        int at = 0;
        while (at < len) {
            int r = is.read(b, at, len - at);
            if (r < 0) throw new IOException("the relay closed the connection");
            at += r;
        }
    }

    private void read(InputStream is) throws IOException {
        ByteArrayOutputStream message = new ByteArrayOutputStream();
        int messageType = 0;
        byte[] h = new byte[8];
        while (state == 1) {
            readFully(is, h, 2);
            boolean fin = (h[0] & 0x80) != 0;
            int op = h[0] & 0x0F;
            boolean masked = (h[1] & 0x80) != 0;
            long len = h[1] & 0x7F;
            if (len == 126) {
                readFully(is, h, 2);
                len = ((h[0] & 0xFF) << 8) | (h[1] & 0xFF);
            } else if (len == 127) {
                readFully(is, h, 8);
                len = 0;
                for (int i = 0; i < 8; i++) len = (len << 8) | (h[i] & 0xFF);
            }
            if (len < 0 || len > MAX_MESSAGE) throw new IOException("a message too long");
            byte[] mask = new byte[4];
            if (masked) readFully(is, mask, 4);
            byte[] data = new byte[(int) len];
            readFully(is, data, (int) len);
            if (masked) for (int i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
            if (op == 0x8) {  // close
                state = 2;
                if (error.isEmpty()) error = "closed by the relay";
                try {
                    frame(0x8, new byte[0]);
                } catch (IOException ignored) {
                }
                socket.close();
                return;
            } else if (op == 0x9) {  // ping
                frame(0xA, data);
            } else if (op == 0x1 || op == 0x2 || op == 0x0) {
                if (op != 0) {
                    messageType = op;
                    message.reset();
                }
                message.write(data, 0, data.length);
                if (message.size() > MAX_MESSAGE) throw new IOException("a message too long");
                if (fin) {
                    if (messageType == 0x1 && in.size() < 2000) in.add(message.toByteArray());
                    message.reset();
                }
            }
        }
    }

    private void frame(int op, byte[] data) throws IOException {
        OutputStream o = out;
        if (o == null) throw new IOException("not connected");
        byte[] mask = new byte[4];
        random.nextBytes(mask);
        ByteArrayOutputStream f = new ByteArrayOutputStream(data.length + 14);
        f.write(0x80 | op);
        if (data.length < 126) f.write(0x80 | data.length);
        else if (data.length < 65536) {
            f.write(0x80 | 126);
            f.write(data.length >> 8);
            f.write(data.length & 0xFF);
        } else {
            f.write(0x80 | 127);
            for (int i = 7; i >= 0; i--) f.write(i >= 4 ? 0 : (data.length >> (8 * i)) & 0xFF);
        }
        f.write(mask, 0, 4);
        for (int i = 0; i < data.length; i++) f.write(data[i] ^ mask[i & 3]);
        synchronized (writing) {
            o.write(f.toByteArray());
            o.flush();
        }
    }
}
