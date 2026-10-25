package com.example.websocketclient;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.IOException;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Protocol;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okhttp3.tls.HandshakeCertificates;
import okhttp3.tls.HeldCertificate;
import okio.ByteString;
import org.junit.Test;

public class WebSocketEndpointTest {
    @Test
    public void bareSecureDomainGetsDefaultRootPathAndTls() {
        Request request = WebSocketEndpoint.request(" wss://www.hongyunz.cc ");
        assertEquals("https://www.hongyunz.cc/", request.url().toString());
        assertEquals(443, request.url().port());
        assertTrue(request.url().isHttps());
        assertEquals("wss://www.hongyunz.cc/", WebSocketEndpoint.displayAddress(request));
    }

    @Test
    public void schemeIsCaseInsensitiveAndPortsPathsAndQueriesArePreserved() {
        Request request = WebSocketEndpoint.request("WSS://www.hongyunz.cc:8443/ws?token=abc");
        assertEquals("wss://www.hongyunz.cc:8443/ws?token=abc", WebSocketEndpoint.displayAddress(request));
        assertEquals("ws://127.0.0.1:8080/", WebSocketEndpoint.displayAddress(
                WebSocketEndpoint.request("ws://127.0.0.1:8080")));
    }

    @Test
    public void secureWebSocketWithoutPathExchangesTextAndBinaryWithVerifiedHostname() throws Exception {
        HeldCertificate certificate = new HeldCertificate.Builder()
                .commonName("localhost").addSubjectAlternativeName("localhost").build();
        HandshakeCertificates serverCertificates = new HandshakeCertificates.Builder()
                .heldCertificate(certificate).build();
        HandshakeCertificates clientCertificates = new HandshakeCertificates.Builder()
                .addTrustedCertificate(certificate.certificate()).build();
        OkHttpClient client = new OkHttpClient.Builder()
                .sslSocketFactory(clientCertificates.sslSocketFactory(), clientCertificates.trustManager())
                .readTimeout(0, TimeUnit.MILLISECONDS).build();
        BlockingQueue<Object> messages = new LinkedBlockingQueue<>();
        try (MockWebServer server = new MockWebServer()) {
            server.useHttps(serverCertificates.sslSocketFactory(), false);
            server.enqueue(new MockResponse().withWebSocketUpgrade(new WebSocketListener() {
                @Override public void onMessage(WebSocket socket, String text) { socket.send(text); }
                @Override public void onMessage(WebSocket socket, ByteString bytes) { socket.send(bytes); }
                @Override public void onClosing(WebSocket socket, int code, String reason) { socket.close(1000, null); }
            }));
            server.start();
            WebSocket socket = client.newWebSocket(WebSocketEndpoint.request("wss://localhost:" + server.getPort()),
                    new WebSocketListener() {
                        @Override public void onOpen(WebSocket socket, Response response) { messages.add("OPEN"); }
                        @Override public void onMessage(WebSocket socket, String text) { messages.add(text); }
                        @Override public void onMessage(WebSocket socket, ByteString bytes) { messages.add(bytes); }
                        @Override public void onFailure(WebSocket socket, Throwable error, Response response) { messages.add(error); }
                    });
            try {
                assertEquals("OPEN", messages.poll(8, TimeUnit.SECONDS));
                String text = "WSS 文本回显";
                ByteString bytes = ByteString.of(new byte[] {0, 1, (byte) 255, 72, 105});
                assertTrue(socket.send(text));
                assertTrue(socket.send(bytes));
                assertEquals(text, messages.poll(8, TimeUnit.SECONDS));
                assertEquals(bytes, messages.poll(8, TimeUnit.SECONDS));
                assertEquals("/", server.takeRequest(8, TimeUnit.SECONDS).getPath());
            } finally {
                socket.cancel();
            }
        } finally {
            client.dispatcher().executorService().shutdown();
            client.connectionPool().evictAll();
        }
    }

    @Test
    public void failedUpgradeExplainsHttpErrorInsteadOfImplyingInvalidWssAddress() {
        Response response = new Response.Builder().request(WebSocketEndpoint.request("wss://www.hongyunz.cc"))
                .protocol(Protocol.HTTP_1_1).code(502).message("Bad Gateway").build();
        String message = WebSocketEndpoint.failureMessage(new IOException("Expected HTTP 101"), response);
        assertTrue(message.contains("HTTP 502 Bad Gateway"));
        assertTrue(message.contains("服务器或网关异常"));
        assertTrue(message.contains("Expected HTTP 101"));
    }
}
