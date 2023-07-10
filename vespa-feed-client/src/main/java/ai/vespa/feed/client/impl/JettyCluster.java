// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

package ai.vespa.feed.client.impl;

import ai.vespa.feed.client.FeedClientBuilder.Compression;
import ai.vespa.feed.client.HttpResponse;
import org.eclipse.jetty.client.HttpClient;
import org.eclipse.jetty.client.HttpProxy;
import org.eclipse.jetty.client.MultiplexConnectionPool;
import org.eclipse.jetty.client.Origin;
import org.eclipse.jetty.client.api.Authentication;
import org.eclipse.jetty.client.api.Request;
import org.eclipse.jetty.client.api.Response;
import org.eclipse.jetty.client.api.Result;
import org.eclipse.jetty.client.util.BufferingResponseListener;
import org.eclipse.jetty.client.util.BytesRequestContent;
import org.eclipse.jetty.http.HttpField;
import org.eclipse.jetty.http.HttpHeader;
import org.eclipse.jetty.http.HttpMethod;
import org.eclipse.jetty.http.HttpVersion;
import org.eclipse.jetty.http2.client.HTTP2Client;
import org.eclipse.jetty.http2.client.http.HttpClientTransportOverHTTP2;
import org.eclipse.jetty.io.ClientConnector;
import org.eclipse.jetty.util.HttpCookieStore;
import org.eclipse.jetty.util.Pool;
import org.eclipse.jetty.util.Promise;
import org.eclipse.jetty.util.SocketAddressResolver;
import org.eclipse.jetty.util.component.AbstractLifeCycle;
import org.eclipse.jetty.util.ssl.SslContextFactory;
import org.eclipse.jetty.util.thread.QueuedThreadPool;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.net.Inet4Address;
import java.net.InetSocketAddress;
import java.net.URI;
import java.time.Duration;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;
import java.util.stream.Collectors;
import java.util.zip.GZIPOutputStream;

import static ai.vespa.feed.client.FeedClientBuilder.Compression.auto;
import static ai.vespa.feed.client.FeedClientBuilder.Compression.gzip;
import static java.util.concurrent.TimeUnit.MILLISECONDS;
import static org.eclipse.jetty.http.MimeTypes.Type.APPLICATION_JSON;

/**
 * Client implementation based on Jetty HTTP Client
 *
 * @author bjorncs
 */
class JettyCluster implements Cluster {

    // Socket timeout must be longer than the longest feasible response timeout
    private static final Duration IDLE_TIMEOUT = Duration.ofMinutes(15);

    private final HttpClient client;
    private final List<Endpoint> endpoints;
    private final Compression compression;

    JettyCluster(FeedClientBuilderImpl b) throws IOException {
        this.client = createHttpClient(b);
        this.endpoints = b.endpoints.stream().map(Endpoint::new).collect(Collectors.toList());
        this.compression = b.compression;
    }

    @Override
    public void dispatch(HttpRequest req, CompletableFuture<HttpResponse> vessel) {
        client.getExecutor().execute(() -> {
            Endpoint endpoint = findLeastBusyEndpoint(endpoints);
            try {
                endpoint.inflight.incrementAndGet();
                long reqTimeoutMillis = req.timeout() != null
                        ? req.timeout().toMillis() * 11 / 10 + 1000 : IDLE_TIMEOUT.toMillis();
                Request jettyReq = client.newRequest(URI.create(endpoint.uri + req.path()))
                        .version(HttpVersion.HTTP_2)
                        .method(HttpMethod.fromString(req.method()))
                        .headers(hs -> req.headers().forEach((k, v) -> {
                            if (!k.equalsIgnoreCase(HttpHeader.PROXY_AUTHORIZATION.asString())) hs.add(k, v.get());
                        }))
                        .idleTimeout(IDLE_TIMEOUT.toMillis(), MILLISECONDS)
                        .timeout(reqTimeoutMillis, MILLISECONDS);
                if (req.body() != null) {
                    boolean shouldCompress = compression == gzip || compression == auto && req.body().length > 512;
                    byte[] bytes;
                    if (shouldCompress) {
                        ByteArrayOutputStream buffer = new ByteArrayOutputStream(1 << 10);
                        try (GZIPOutputStream zip = new GZIPOutputStream(buffer)) {
                            zip.write(req.body());
                        } catch (IOException e) { throw new UncheckedIOException(e); }
                        bytes = buffer.toByteArray();
                        jettyReq.headers(hs -> hs.add(HttpHeader.CONTENT_ENCODING, "gzip"));
                    } else {
                        bytes = req.body();
                    }
                    jettyReq.body(new BytesRequestContent(APPLICATION_JSON.asString(), bytes));
                }
                jettyReq.send(new BufferingResponseListener() {
                    @Override
                    public void onComplete(Result result) {
                        endpoint.inflight.decrementAndGet();
                        if (result.isFailed()) vessel.completeExceptionally(result.getFailure());
                        else vessel.complete(new JettyResponse(result.getResponse(), getContent()));
                    }
                });
            } catch (Exception e) {
                endpoint.inflight.decrementAndGet();
                vessel.completeExceptionally(e);
            }
        });
    }

    @Override
    public void close() {
        try {
            client.stop();
        } catch (Exception e) { throw new RuntimeException(e); }
    }

    private static HttpClient createHttpClient(FeedClientBuilderImpl b) throws IOException {
        SslContextFactory.Client clientSslCtxFactory = new SslContextFactory.Client();
        clientSslCtxFactory.setSslContext(b.constructSslContext());
        if (b.hostnameVerifier != null) {
            clientSslCtxFactory.setHostnameVerifier(b.hostnameVerifier);
            // Disable built-in hostname verification in the JDK's TLS implementation
            clientSslCtxFactory.setEndpointIdentificationAlgorithm(null);
        }
        ClientConnector connector = new ClientConnector();
        int threads = Math.max(Math.min(Runtime.getRuntime().availableProcessors(), 32), 8);
        connector.setExecutor(new QueuedThreadPool(threads));
        connector.setSslContextFactory(clientSslCtxFactory);
        connector.setIdleTimeout(IDLE_TIMEOUT);
        connector.setConnectTimeout(Duration.ofSeconds(10));
        HTTP2Client h2Client = new HTTP2Client(connector);
        h2Client.setMaxConcurrentPushedStreams(b.maxStreamsPerConnection);
        // Set the HTTP/2 flow control windows very large to cause TCP congestion instead of HTTP/2 flow control congestion.
        int initialWindow = Integer.MAX_VALUE;
        h2Client.setInitialSessionRecvWindow(initialWindow);
        h2Client.setInitialStreamRecvWindow(initialWindow);
        HttpClientTransportOverHTTP2 transport = new HttpClientTransportOverHTTP2(h2Client);
        transport.setConnectionPoolFactory(dest -> {
            MultiplexConnectionPool pool = new MultiplexConnectionPool(
                    dest, Pool.StrategyType.RANDOM, b.connectionsPerEndpoint, false, dest, Integer.MAX_VALUE);
            pool.preCreateConnections(b.connectionsPerEndpoint);
            return pool;
        });
        HttpClient httpClient = new HttpClient(transport);
        httpClient.setFollowRedirects(false);
        httpClient.setUserAgentField(
                new HttpField(HttpHeader.USER_AGENT, String.format("vespa-feed-client/%s (Jetty)", Vespa.VERSION)));
        // Stop client from trying different IP address when TLS handshake fails
        httpClient.setSocketAddressResolver(new Ipv4PreferringResolver(httpClient, Duration.ofSeconds(10)));
        httpClient.setCookieStore(new HttpCookieStore.Empty());

        if (b.proxy != null) addProxyConfiguration(b, httpClient);
        try { httpClient.start(); } catch (Exception e) { throw new IOException(e); }
        return httpClient;
    }

    private static void addProxyConfiguration(FeedClientBuilderImpl b, HttpClient httpClient) throws IOException {
        Origin.Address address = new Origin.Address(b.proxy.getHost(), b.proxy.getPort());
        if (b.proxy.getScheme().equals("https")) {
            SslContextFactory.Client proxySslCtxFactory = new SslContextFactory.Client();
            if (b.hostnameVerifier != null) proxySslCtxFactory.setHostnameVerifier(b.hostnameVerifier);
            // Disable built-in hostname verification in the JDK's TLS implementation
            proxySslCtxFactory.setEndpointIdentificationAlgorithm(null);
            try { proxySslCtxFactory.start(); } catch (Exception e) { throw new IOException(e); }
            httpClient.getProxyConfiguration().addProxy(
                    new HttpProxy(address, proxySslCtxFactory, new Origin.Protocol(Collections.singletonList("h2"), false)));
        } else {
            httpClient.getProxyConfiguration().addProxy(
                    new HttpProxy(address, false, new Origin.Protocol(Collections.singletonList("h2c"), false)));
        }
        Map<String, Supplier<String>> proxyHeaders = new TreeMap<>();
        b.requestHeaders.forEach((k, v) -> { if (isProxyHeader(k)) proxyHeaders.put(k, v); });
        if (!proxyHeaders.isEmpty()) {
            for (URI endpoint : b.endpoints) {
                httpClient.getAuthenticationStore().addAuthenticationResult(new Authentication.Result() {
                    @Override public URI getURI() { return URI.create(endpointUri(endpoint)); }
                    @Override public void apply(Request r) {
                        r.headers(hs -> proxyHeaders.forEach((k, v) -> hs.add(k, v.get())));
                    }
                });

            }
        }
    }

    private static boolean isProxyHeader(String h) { return h.equalsIgnoreCase(HttpHeader.PROXY_AUTHORIZATION.asString()); }

    private static Endpoint findLeastBusyEndpoint(List<Endpoint> endpoints) {
        Endpoint leastBusy = endpoints.get(0);
        int minInflight = leastBusy.inflight.get();
        for (int i = 1; i < endpoints.size(); i++) {
            Endpoint endpoint = endpoints.get(i);
            int inflight = endpoint.inflight.get();
            if (inflight < minInflight) {
                leastBusy = endpoint;
                minInflight = inflight;
            }
        }
        return leastBusy;
    }

    private static int portOf(URI u) {
        return u.getPort() == -1 ? u.getScheme().equals("http") ? 80 : 443 : u.getPort();
    }

    private static String endpointUri(URI uri) {
        return String.format("%s://%s:%s", uri.getScheme(), uri.getHost(), portOf(uri));
    }

    private static class JettyResponse implements HttpResponse {
        final Response response;
        final byte[] content;

        JettyResponse(Response response, byte[] content) { this.response = response; this.content = content; }

        @Override public int code() { return response.getStatus(); }
        @Override public byte[] body() { return content; }
        @Override public String contentType() { return response.getHeaders().get(HttpHeader.CONTENT_TYPE); }
    }

    private static class Endpoint {
        final AtomicInteger inflight = new AtomicInteger();
        final String uri;
        Endpoint(URI uri) { this.uri = endpointUri(uri); }
    }

    private static class Ipv4PreferringResolver extends AbstractLifeCycle implements SocketAddressResolver {

        final HttpClient client;
        final Duration timeout;
        SocketAddressResolver.Async instance;

        Ipv4PreferringResolver(HttpClient client, Duration timeout) { this.client = client; this.timeout = timeout; }

        @Override
        protected void doStart() {
            this.instance = new SocketAddressResolver.Async(client.getExecutor(), client.getScheduler(), timeout.toMillis());
        }

        @Override
        public void resolve(String host, int port, Promise<List<InetSocketAddress>> promise) {
            instance.resolve(host, port, new Promise.Wrapper<List<InetSocketAddress>>(promise) {
                @Override
                public void succeeded(List<InetSocketAddress> result) {
                    if (result.size() <= 1) {
                        getPromise().succeeded(result);
                        return;
                    }
                    List<InetSocketAddress> ipv4Addresses = result.stream()
                            .filter(addr -> addr.getAddress() instanceof Inet4Address).collect(Collectors.toList());
                    if (ipv4Addresses.isEmpty()) {
                        getPromise().succeeded(result);
                        return;
                    }
                    getPromise().succeeded(ipv4Addresses);
                }
            });
        }
    }
}
