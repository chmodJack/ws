package com.example.websocketclient;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.view.MotionEvent;
import android.view.WindowInsetsController;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

public class MainActivity extends Activity {
    private static final int BG = Color.rgb(246, 247, 251);
    private static final int CARD = Color.WHITE;
    private static final int TEXT = Color.rgb(23, 32, 51);
    private static final int MUTED = Color.rgb(102, 112, 133);
    private static final int PRIMARY = Color.rgb(79, 70, 229);
    private static final int GREEN = Color.rgb(19, 138, 82);
    private static final int RED = Color.rgb(201, 54, 54);

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final OkHttpClient httpClient = new OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .build();

    private EditText endpointInput;
    private EditText textInput;
    private EditText binaryInput;
    private TextView statusText;
    private TextView statusDot;
    private TextView logText;
    private ScrollView logScroll;
    private Button connectButton;
    private Button sendTextButton;
    private Button sendBinaryButton;
    private WebSocket webSocket;
    private boolean connected;
    private boolean destroyed;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        configureSystemBars();
        setContentView(createContent());
    }

    private void configureSystemBars() {
        Window window = getWindow();
        window.setStatusBarColor(BG);
        window.setNavigationBarColor(BG);
        if (android.os.Build.VERSION.SDK_INT >= 23) {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        }
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.setSystemBarsAppearance(WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS,
                        WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS);
            }
        }
    }

    private View createContent() {
        ScrollView scrollView = new ScrollView(this);
        scrollView.setFillViewport(true);
        scrollView.setBackgroundColor(BG);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(18), dp(20), dp(28));
        scrollView.addView(content, new ScrollView.LayoutParams(-1, -2));

        TextView eyebrow = label("NETWORK TOOL", 12, MUTED);
        eyebrow.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        content.addView(eyebrow, marginParams(-1, -2, 0, 0, 0, 6));

        TextView title = label("WebSocket Client", 28, TEXT);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        content.addView(title, marginParams(-1, -2, 0, 0, 0, 4));

        TextView subtitle = label("连接服务器，分别收发文本与二进制消息", 14, MUTED);
        content.addView(subtitle, marginParams(-1, -2, 0, 0, 0, 18));

        content.addView(createConnectionCard(), marginParams(-1, -2, 0, 0, 0, 14));
        content.addView(createSendCard(), marginParams(-1, -2, 0, 0, 0, 14));
        content.addView(createLogCard(), marginParams(-1, -2, 0, 0, 0, 0));

        return scrollView;
    }

    private View createConnectionCard() {
        LinearLayout card = card();
        card.addView(sectionTitle("连接设置", "输入 ws:// 或 wss:// 地址"), marginParams(-1, -2, 0, 0, 0, 14));

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setOrientation(LinearLayout.HORIZONTAL);

        endpointInput = editText("wss://www.hongyunz.cc", false);
        endpointInput.setId(R.id.endpoint_input);
        endpointInput.setSingleLine(true);
        endpointInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        endpointInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        row.addView(endpointInput, new LinearLayout.LayoutParams(0, dp(50), 1f));

        connectButton = button("连接", PRIMARY);
        connectButton.setId(R.id.connect_button);
        connectButton.setOnClickListener(v -> toggleConnection());
        LinearLayout.LayoutParams connectParams = new LinearLayout.LayoutParams(dp(86), dp(50));
        connectParams.leftMargin = dp(10);
        row.addView(connectButton, connectParams);
        card.addView(row);

        LinearLayout statusRow = new LinearLayout(this);
        statusRow.setGravity(Gravity.CENTER_VERTICAL);
        statusDot = label("●", 18, RED);
        statusDot.setGravity(Gravity.CENTER);
        statusRow.addView(statusDot, new LinearLayout.LayoutParams(dp(24), dp(28)));
        statusText = label("未连接", 14, MUTED);
        statusText.setId(R.id.connection_status);
        statusRow.addView(statusText, marginParams(-2, dp(28), 4, 10, 0, 0));
        card.addView(statusRow);
        return card;
    }

    private View createSendCard() {
        LinearLayout card = card();
        card.addView(sectionTitle("发送消息", "选择对应格式发送到当前连接"), marginParams(-1, -2, 0, 0, 0, 14));

        TextView textLabel = label("文本消息", 13, TEXT);
        textLabel.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        card.addView(textLabel, marginParams(-1, -2, 0, 0, 0, 6));

        textInput = editText("输入要发送的文本…", true);
        textInput.setId(R.id.text_input);
        textInput.setMinHeight(dp(92));
        card.addView(textInput, marginParams(-1, -2, 0, 0, 0, 8));

        sendTextButton = button("发送文本", PRIMARY);
        sendTextButton.setId(R.id.send_text);
        sendTextButton.setOnClickListener(v -> sendTextMessage());
        card.addView(sendTextButton, alignedButtonParams(0, 16));

        TextView binaryLabel = label("二进制消息（十六进制）", 13, TEXT);
        binaryLabel.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        card.addView(binaryLabel, marginParams(-1, -2, 0, 20, 0, 6));

        binaryInput = editText("例如：48 65 6C 6C 6F", true);
        binaryInput.setId(R.id.binary_input);
        binaryInput.setTypeface(Typeface.MONOSPACE);
        binaryInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        binaryInput.setMinHeight(dp(76));
        card.addView(binaryInput, marginParams(-1, -2, 0, 0, 0, 8));

        TextView binaryHint = label("支持空格、冒号或连字符分隔；每两个字符代表一个字节", 12, MUTED);
        card.addView(binaryHint, marginParams(-1, -2, 0, 0, 0, 8));

        sendBinaryButton = button("发送二进制", PRIMARY);
        sendBinaryButton.setId(R.id.send_binary);
        sendBinaryButton.setOnClickListener(v -> sendBinaryMessage());
        card.addView(sendBinaryButton, alignedButtonParams(0, 0));
        updateConnectedUi();
        return card;
    }

    private View createLogCard() {
        LinearLayout card = card();
        LinearLayout titleRow = new LinearLayout(this);
        titleRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = label("收发日志", 18, TEXT);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        titleRow.addView(title, new LinearLayout.LayoutParams(0, -2, 1f));
        Button clear = smallButton("清空");
        clear.setId(R.id.clear_log);
        clear.setOnClickListener(v -> {
            logText.setText("");
            logScroll.scrollTo(0, 0);
        });
        titleRow.addView(clear, new LinearLayout.LayoutParams(dp(68), dp(36)));
        card.addView(titleRow, marginParams(-1, -2, 0, 0, 0, 12));

        logScroll = new ScrollView(this);
        logScroll.setId(R.id.log_scroll);
        logScroll.setFillViewport(true);
        logScroll.setBackground(roundRect(Color.rgb(15, 23, 42), 12, 0, Color.TRANSPARENT));
        // Keep gestures inside the log viewport instead of scrolling the outer form.
        logScroll.setOnTouchListener((view, event) -> {
            int action = event.getActionMasked();
            view.getParent().requestDisallowInterceptTouchEvent(
                    action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL);
            return false;
        });

        logText = new TextView(this);
        logText.setId(R.id.log_text);
        logText.setTextColor(Color.rgb(226, 232, 240));
        logText.setTextSize(12);
        logText.setTypeface(Typeface.MONOSPACE);
        logText.setGravity(Gravity.TOP | Gravity.START);
        logText.setPadding(dp(12), dp(12), dp(12), dp(12));
        logText.setText("准备就绪，连接后消息会显示在这里。\n");
        logText.addOnLayoutChangeListener((view, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> {
            if (bottom - top != oldBottom - oldTop) logScroll.scrollTo(0, bottom - top);
        });
        logScroll.addView(logText, new ScrollView.LayoutParams(-1, -2));
        card.addView(logScroll, new LinearLayout.LayoutParams(-1, dp(220)));
        return card;
    }

    private void toggleConnection() {
        if (connected || webSocket != null) {
            closeSocket();
            return;
        }
        Request request;
        try {
            request = WebSocketEndpoint.request(endpointInput.getText().toString());
        } catch (IllegalArgumentException exception) {
            setStatus(false, "地址格式错误");
            appendLog("系统", "错误", exception.getMessage());
            return;
        }
        String address = WebSocketEndpoint.displayAddress(request);
        endpointInput.setText(address);
        setStatus(false, "连接中…");
        appendLog("系统", "连接", address);
        webSocket = httpClient.newWebSocket(request, new WebSocketListener() {
            @Override
            public void onOpen(WebSocket socket, Response response) {
                postForSocket(socket, () -> {
                    connected = true;
                    setStatus(true, "已连接");
                    appendLog("系统", "已连接", address);
                    updateConnectedUi();
                });
            }

            @Override
            public void onMessage(WebSocket socket, String text) {
                postForSocket(socket, () -> appendLog("接收", "文本", text));
            }

            @Override
            public void onMessage(WebSocket socket, ByteString bytes) {
                postForSocket(socket, () -> appendLog("接收", "二进制 · " + bytes.size() + " bytes", formatHex(bytes.toByteArray())));
            }

            @Override
            public void onClosing(WebSocket socket, int code, String reason) {
                postForSocket(socket, () -> {
                    connected = false;
                    setStatus(false, "服务器关闭连接");
                    appendLog("系统", "关闭中", "code=" + code + (reason.isEmpty() ? "" : ", " + reason));
                    socket.close(1000, null);
                    updateConnectedUi();
                });
            }

            @Override
            public void onClosed(WebSocket socket, int code, String reason) {
                postForSocket(socket, () -> {
                    webSocket = null;
                    connected = false;
                    setStatus(false, "已断开");
                    appendLog("系统", "已关闭", "code=" + code + (reason.isEmpty() ? "" : ", " + reason));
                    updateConnectedUi();
                });
            }

            @Override
            public void onFailure(WebSocket socket, Throwable t, Response response) {
                postForSocket(socket, () -> {
                    webSocket = null;
                    connected = false;
                    setStatus(false, response == null ? "连接失败" : "连接失败 · HTTP " + response.code());
                    appendLog("系统", "连接失败", WebSocketEndpoint.failureMessage(t, response));
                    updateConnectedUi();
                });
            }
        });
        updateConnectedUi();
    }

    private void postForSocket(WebSocket socket, Runnable action) {
        mainHandler.post(() -> {
            if (!destroyed && webSocket == socket) action.run();
        });
    }

    private void closeSocket() {
        WebSocket socket = webSocket;
        webSocket = null;
        if (socket != null) {
            if (connected) socket.close(1000, "用户断开");
            else socket.cancel();
        }
        connected = false;
        setStatus(false, "已断开");
        appendLog("系统", "已断开", "用户断开连接");
        updateConnectedUi();
    }

    private void sendTextMessage() {
        if (!ensureConnected()) return;
        String text = textInput.getText().toString();
        if (text.isEmpty()) {
            appendLog("系统", "提示", "请输入文本消息");
            return;
        }
        if (webSocket.send(text)) {
            appendLog("发送", "文本", text);
        } else {
            appendLog("系统", "错误", "文本消息未能加入发送队列");
        }
    }

    private void sendBinaryMessage() {
        if (!ensureConnected()) return;
        String raw = binaryInput.getText().toString().trim();
        if (raw.isEmpty()) {
            appendLog("系统", "提示", "请输入十六进制字节");
            return;
        }
        try {
            byte[] bytes = parseHex(raw);
            if (webSocket.send(ByteString.of(bytes))) {
                appendLog("发送", "二进制 · " + bytes.length + " bytes", formatHex(bytes));
            } else {
                appendLog("系统", "错误", "二进制消息未能加入发送队列");
            }
        } catch (IllegalArgumentException exception) {
            appendLog("系统", "错误", exception.getMessage());
        }
    }

    private boolean ensureConnected() {
        if (!connected || webSocket == null) {
            appendLog("系统", "提示", "请先连接 WebSocket 服务器");
            return false;
        }
        return true;
    }

    private byte[] parseHex(String value) {
        String normalized = value.replace("0x", "").replace("0X", "")
                .replaceAll("[\\s,:-]", "");
        if (normalized.isEmpty()) throw new IllegalArgumentException("请输入有效的十六进制字节");
        if ((normalized.length() & 1) != 0) throw new IllegalArgumentException("十六进制内容必须是偶数个字符");
        if (!normalized.matches("[0-9a-fA-F]+")) throw new IllegalArgumentException("包含无效的十六进制字符");
        byte[] result = new byte[normalized.length() / 2];
        for (int i = 0; i < result.length; i++) {
            result[i] = (byte) Integer.parseInt(normalized.substring(i * 2, i * 2 + 2), 16);
        }
        return result;
    }

    private String formatHex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 3);
        for (int i = 0; i < bytes.length; i++) {
            if (i > 0) result.append(' ');
            result.append(String.format(Locale.US, "%02X", bytes[i] & 0xff));
        }
        return result.toString();
    }

    private void appendLog(String direction, String type, String message) {
        if (logText == null) return;
        String time = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date());
        logText.append("[" + time + "] " + direction + "  " + type + "\n" + message + "\n\n");
        // Scroll only the dedicated viewport; scrolling the card hides its contents.
        logScroll.post(() -> logScroll.scrollTo(0, logText.getHeight()));
    }

    private void setStatus(boolean isConnected, String message) {
        if (statusText == null) return;
        statusText.setText(message);
        statusText.setTextColor(isConnected ? GREEN : MUTED);
        statusDot.setTextColor(isConnected ? GREEN : RED);
    }

    private void updateConnectedUi() {
        boolean enabled = connected && webSocket != null;
        boolean hasSocket = webSocket != null;
        endpointInput.setEnabled(!hasSocket);
        connectButton.setText(hasSocket ? (connected ? "断开" : "取消") : "连接");
        connectButton.setBackgroundTintList(ColorStateList.valueOf(hasSocket ? RED : PRIMARY));
        if (sendTextButton != null) {
            sendTextButton.setEnabled(enabled);
            sendBinaryButton.setEnabled(enabled);
            int color = enabled ? PRIMARY : Color.rgb(177, 182, 198);
            sendTextButton.setBackgroundTintList(ColorStateList.valueOf(color));
            sendBinaryButton.setBackgroundTintList(ColorStateList.valueOf(color));
        }
    }

    private LinearLayout card() {
        LinearLayout result = new LinearLayout(this);
        result.setOrientation(LinearLayout.VERTICAL);
        result.setPadding(dp(16), dp(16), dp(16), dp(16));
        result.setBackground(roundRect(CARD, 16, 1, Color.rgb(232, 234, 240)));
        return result;
    }

    private View sectionTitle(String title, String subtitle) {
        LinearLayout result = new LinearLayout(this);
        result.setOrientation(LinearLayout.VERTICAL);
        TextView titleView = label(title, 18, TEXT);
        titleView.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        result.addView(titleView);
        result.addView(label(subtitle, 12, MUTED), marginParams(-1, -2, 0, 4, 0, 0));
        return result;
    }

    private TextView label(String text, float size, int color) {
        TextView result = new TextView(this);
        result.setText(text);
        result.setTextSize(size);
        result.setTextColor(color);
        return result;
    }

    private EditText editText(String hint, boolean multiline) {
        EditText result = new EditText(this);
        result.setHint(hint);
        result.setHintTextColor(Color.rgb(152, 162, 179));
        result.setTextColor(TEXT);
        result.setTextSize(14);
        result.setPadding(dp(13), dp(10), dp(13), dp(10));
        result.setBackground(roundRect(Color.WHITE, 10, 1, Color.rgb(208, 213, 221)));
        if (multiline) {
            result.setGravity(Gravity.TOP | Gravity.START);
            result.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        }
        return result;
    }

    private Button button(String text, int color) {
        Button result = new Button(this);
        result.setText(text);
        result.setTextSize(14);
        result.setTextColor(Color.WHITE);
        result.setAllCaps(false);
        result.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        result.setPadding(dp(8), 0, dp(8), 0);
        result.setBackgroundTintList(ColorStateList.valueOf(color));
        return result;
    }

    private Button smallButton(String text) {
        Button result = button(text, Color.rgb(235, 237, 245));
        result.setTextColor(MUTED);
        result.setTypeface(Typeface.DEFAULT, Typeface.NORMAL);
        return result;
    }

    private LinearLayout.LayoutParams alignedButtonParams(int top, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, dp(46));
        params.gravity = Gravity.END;
        params.topMargin = dp(top);
        params.bottomMargin = dp(bottom);
        return params;
    }

    private LinearLayout.LayoutParams marginParams(int width, int height, int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(width, height);
        params.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return params;
    }

    private GradientDrawable roundRect(int color, int radius, int strokeWidth, int strokeColor) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radius));
        if (strokeWidth > 0) drawable.setStroke(dp(strokeWidth), strokeColor);
        return drawable;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        mainHandler.removeCallbacksAndMessages(null);
        if (webSocket != null) {
            webSocket.cancel();
            webSocket = null;
        }
        httpClient.dispatcher().executorService().shutdown();
        super.onDestroy();
    }
}
