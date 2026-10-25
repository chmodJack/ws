package com.example.websocketclient;

import java.net.ConnectException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;

import javax.net.ssl.SSLException;

import okhttp3.HttpUrl;
import okhttp3.Request;
import okhttp3.Response;

final class WebSocketEndpoint {
    private WebSocketEndpoint() {}

    static Request request(String input) {
        String address = input.trim();
        String httpAddress;
        if (address.regionMatches(true, 0, "wss://", 0, 6)) {
            httpAddress = "https://" + address.substring(6);
        } else if (address.regionMatches(true, 0, "ws://", 0, 5)) {
            httpAddress = "http://" + address.substring(5);
        } else {
            throw new IllegalArgumentException("地址必须以 ws:// 或 wss:// 开头，例如 wss://www.hongyunz.cc");
        }
        HttpUrl url = HttpUrl.parse(httpAddress);
        if (url == null) throw new IllegalArgumentException("地址格式错误，请检查域名、端口和路径");
        // HttpUrl supplies '/' for a bare domain, preserving ports, paths and queries.
        // HTTPS keeps OkHttp's standard TLS certificate and hostname verification.
        return new Request.Builder().url(url).build();
    }

    static String displayAddress(Request request) {
        String url = request.url().toString();
        return request.url().isHttps() ? "wss://" + url.substring(8) : "ws://" + url.substring(7);
    }

    static String failureMessage(Throwable failure, Response response) {
        String detail = failure.getMessage();
        if (detail == null || detail.isEmpty()) detail = failure.getClass().getSimpleName();
        if (response != null) {
            int code = response.code();
            String hint;
            if (code >= 500) {
                hint = "服务器或网关异常，请检查 WebSocket 服务及反向代理配置。";
            } else if (code == 401 || code == 403) {
                hint = "服务器拒绝连接，请检查访问权限。";
            } else {
                hint = "服务器未完成 WebSocket 升级，请确认地址及路径（例如 /ws）。";
            }
            String location = response.header("Location");
            return "HTTP " + code + " " + response.message() + "\n" + hint
                    + (location == null ? "" : "\n重定向地址：" + location) + "\n" + detail;
        }
        String hint = "";
        if (failure instanceof SSLException) {
            hint = "TLS 握手或证书校验失败，请检查服务器证书及设备时间。\n";
        } else if (failure instanceof UnknownHostException) {
            hint = "域名解析失败，请检查地址和网络。\n";
        } else if (failure instanceof ConnectException) {
            hint = "无法连接服务器，请检查网络、端口及服务状态。\n";
        } else if (failure instanceof SocketTimeoutException) {
            hint = "连接超时，请检查网络及服务状态。\n";
        }
        return hint + detail;
    }
}
