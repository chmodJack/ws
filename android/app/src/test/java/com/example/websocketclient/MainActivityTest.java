package com.example.websocketclient;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.robolectric.Shadows.shadowOf;

import android.os.Looper;
import android.view.View;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okhttp3.mockwebserver.MockResponse;
import okhttp3.mockwebserver.MockWebServer;
import okio.ByteString;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 28)
@LooperMode(LooperMode.Mode.PAUSED)
public class MainActivityTest {
    private MockWebServer server;
    private ActivityController<MainActivity> controller;
    private MainActivity activity;

    @Before
    public void connectToEchoServer() throws Exception {
        server = new MockWebServer();
        server.enqueue(new MockResponse().withWebSocketUpgrade(new WebSocketListener() {
            @Override public void onMessage(WebSocket socket, String text) {
                socket.send(text);
            }

            @Override public void onMessage(WebSocket socket, ByteString bytes) {
                socket.send(bytes);
            }

            @Override public void onClosing(WebSocket socket, int code, String reason) {
                socket.close(1000, null);
            }
        }));
        server.start();
        controller = Robolectric.buildActivity(MainActivity.class).setup().visible();
        activity = controller.get();
        EditText endpoint = activity.findViewById(R.id.endpoint_input);
        endpoint.setText("ws://" + server.getHostName() + ":" + server.getPort());
        activity.findViewById(R.id.connect_button).performClick();
        waitUntil(() -> "已连接".contentEquals(status().getText()));
    }

    @After
    public void stop() throws Exception {
        if (controller != null) controller.pause().stop().destroy();
        shadowOf(Looper.getMainLooper()).idle();
        if (server != null) server.close();
    }

    @Test
    public void textCanBeSentTwiceWithoutClearingAndBothDirectionsAreLogged() throws Exception {
        String message = "Hello 文本 🌍";
        EditText input = activity.findViewById(R.id.text_input);
        input.setText(message);
        activity.findViewById(R.id.send_text).performClick();
        waitUntil(() -> logs().contains("接收  文本\n" + message));
        assertEquals(message, input.getText().toString());
        activity.findViewById(R.id.send_text).performClick();
        waitUntil(() -> occurrences(logs(), "接收  文本\n" + message) == 2);
        assertEquals(message, input.getText().toString());
        assertEquals(2, occurrences(logs(), "发送  文本\n" + message));
    }

    @Test
    public void binaryCanBeSentTwiceWithoutClearingAndBothDirectionsAreLogged() throws Exception {
        String inputValue = "0x00:FF-48 65 6c 6c 6f";
        String expectedHex = "00 FF 48 65 6C 6C 6F";
        EditText input = activity.findViewById(R.id.binary_input);
        input.setText(inputValue);
        activity.findViewById(R.id.send_binary).performClick();
        String received = "接收  二进制 · 7 bytes\n" + expectedHex;
        waitUntil(() -> logs().contains(received));
        assertEquals(inputValue, input.getText().toString());
        activity.findViewById(R.id.send_binary).performClick();
        waitUntil(() -> occurrences(logs(), received) == 2);
        assertEquals(inputValue, input.getText().toString());
        assertEquals(2, occurrences(logs(), "发送  二进制 · 7 bytes\n" + expectedHex));
    }

    @Test
    public void longLogsScrollInsideViewportAndTheCardRemainsVisible() throws Exception {
        EditText input = activity.findViewById(R.id.text_input);
        input.setText("scroll-regression");
        for (int i = 0; i < 60; i++) activity.findViewById(R.id.send_text).performClick();
        waitUntil(() -> occurrences(logs(), "接收  文本\nscroll-regression") == 60);
        View decor = activity.getWindow().getDecorView();
        decor.measure(View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(1920, View.MeasureSpec.EXACTLY));
        decor.layout(0, 0, 1080, 1920);
        shadowOf(Looper.getMainLooper()).idle();

        ScrollView viewport = activity.findViewById(R.id.log_scroll);
        TextView text = activity.findViewById(R.id.log_text);
        assertTrue("logs should exceed the fixed viewport", text.getHeight() > viewport.getHeight());
        assertTrue("latest messages should be visible", viewport.getScrollY() > 0);
        assertEquals("the containing card must never be scrolled", 0,
                ((View) viewport.getParent()).getScrollY());
        assertTrue("the bottom of the log should be inside the viewport",
                text.getBottom() - viewport.getScrollY() <= viewport.getHeight());

        activity.findViewById(R.id.clear_log).performClick();
        assertEquals("", logs());
    }

    private TextView status() {
        return activity.findViewById(R.id.connection_status);
    }

    private String logs() {
        return ((TextView) activity.findViewById(R.id.log_text)).getText().toString();
    }

    private void waitUntil(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(8);
        do {
            shadowOf(Looper.getMainLooper()).idle();
            if (condition.getAsBoolean()) return;
            Thread.sleep(10);
        } while (System.nanoTime() < deadline);
        throw new AssertionError("Timed out. Status: " + status().getText() + "\n" + logs());
    }

    private static int occurrences(String value, String needle) {
        int count = 0;
        for (int index = 0; (index = value.indexOf(needle, index)) >= 0; index += needle.length()) count++;
        return count;
    }
}
