# Multi-stage build for Selfvoy AI Core (Go 1.23+)
FROM golang:1.23-alpine AS builder

WORKDIR /build

COPY go.mod go.sum* ./
RUN go mod download

COPY cmd/ ./cmd/
COPY pkg/ ./pkg/
COPY internal/ ./internal/

RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags="-w -s" -o /ai-core ./cmd/ai-core

# Minimal production image
FROM alpine:3.20

RUN apk --no-cache add ca-certificates tzdata

WORKDIR /app
COPY --from=builder /ai-core /app/ai-core

USER 10001:10001

EXPOSE 8000

ENTRYPOINT ["/app/ai-core"]
