FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod main.go ./
RUN CGO_ENABLED=0 go build -o /eva-todo-server .
FROM alpine:3.20
COPY --from=build /eva-todo-server /usr/local/bin/eva-todo-server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/eva-todo-server"]
