// Copyright 2020 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package ai.vespa.metricsproxy.service;

import ai.vespa.util.http.hc5.VespaAsyncHttpClientBuilder;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.logging.Level;

import com.yahoo.yolean.Exceptions;
import org.apache.hc.client5.http.async.methods.SimpleHttpRequest;
import org.apache.hc.client5.http.async.methods.SimpleHttpResponse;
import org.apache.hc.client5.http.config.RequestConfig;
import org.apache.hc.client5.http.impl.async.CloseableHttpAsyncClient;
import org.apache.hc.core5.util.Timeout;

import java.io.IOException;
import java.net.URI;
import java.util.logging.Logger;

/**
 * HTTP client to get metrics or health data from a service
 *
 * @author hmusum
 * @author bjorncs
 */
public abstract class HttpMetricFetcher {

    private final static Logger log = Logger.getLogger(HttpMetricFetcher.class.getPackage().getName());
    public final static String STATE_PATH = "/state/v1/";
    // The call to apache will do 3 retries. As long as we check the services in series, we can't have this too high.
    public static int CONNECTION_TIMEOUT = 5000;
    private final static int SOCKET_TIMEOUT = 60000;
    private final URI url;
    protected final VespaService service;
    private static final CloseableHttpAsyncClient httpClient = createHttpClient();

    /**
     * @param service the service to fetch metrics from
     * @param port    the port to use
     */
    HttpMetricFetcher(VespaService service, int port, String path) {
        this.service = service;

        String u = "http://localhost:" + port + path;
        this.url = URI.create(u);
        log.log(Level.FINE, "Fetching metrics from " + u + " with timeout " + CONNECTION_TIMEOUT);
    }

    byte [] getJson() throws IOException {
        log.log(Level.FINE, "Connecting to url " + url + " for service '" + service + "'");
        Future<SimpleHttpResponse> response = httpClient.execute(new SimpleHttpRequest("GET", url), null);
        try {
            return response.get().getBodyBytes();
        } catch (InterruptedException | ExecutionException e) {
            throw new IOException("Failed fetching '" + url + "': " + e);
        }
    }

    public String toString() {
        return this.getClass().getSimpleName() + " using " + url;
    }

    String errMsgNoResponse(IOException e) {
        return "Unable to get response from service '" + service + "': " +
                Exceptions.toMessageString(e);
    }

    void handleException(Exception e, Object data, int timesFetched) {
        logMessage("Unable to parse json '" + data + "' for service '" + service + "': " +
                           Exceptions.toMessageString(e), timesFetched);
    }

    private void logMessage(String message, int timesFetched) {
        if (service.isAlive() && timesFetched > 5) {
            log.log(Level.INFO, message);
        } else {
            log.log(Level.FINE, message);
        }
    }

    void logMessageNoResponse(String message, int timesFetched) {
        if (timesFetched > 5) {
            log.log(Level.WARNING, message);
        } else {
            log.log(Level.INFO, message);
        }
    }

    private static CloseableHttpAsyncClient createHttpClient() {
        CloseableHttpAsyncClient client =  VespaAsyncHttpClientBuilder.create()
                .setUserAgent("metrics-proxy-http-client")
                .setDefaultRequestConfig(RequestConfig.custom()
                                                 .setConnectTimeout(Timeout.ofMilliseconds(CONNECTION_TIMEOUT))
                                                 .setResponseTimeout(Timeout.ofMilliseconds(SOCKET_TIMEOUT))
                                                 .build())
                .build();
        client.start();
        return client;
    }

}
