FROM alpine:latest

WORKDIR /app

COPY rtsproxy /app/rtsproxy

# Standalone configuration and dashboard are looked up relative to WORKDIR.
COPY config.toml /app/config.toml
COPY webui /app/webui

RUN chmod +x /app/rtsproxy

EXPOSE 8554

ENTRYPOINT ["/app/rtsproxy", "-c", "/app/config.toml"]
