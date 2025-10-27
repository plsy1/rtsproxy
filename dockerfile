FROM alpine:latest

WORKDIR /app

COPY rtsproxy /app/rtsproxy

RUN chmod +x /app/rtsproxy

EXPOSE 8554

ENTRYPOINT ["/app/rtsproxy"]