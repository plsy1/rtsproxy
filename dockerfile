FROM alpine:latest

WORKDIR /app

COPY rtsproxy /app/rtsproxy

# Without config.json the built-in blacklist is empty and every upstream is
# allowed; without webui/ the /admin/ dashboard 404s. Both are looked up
# relative to WORKDIR at runtime.
COPY config.json /app/config.json
COPY webui /app/webui

RUN chmod +x /app/rtsproxy

EXPOSE 8554

ENTRYPOINT ["/app/rtsproxy"]