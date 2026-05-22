# syntax=docker/dockerfile:1

ARG FPS_BASE_IMAGE=fps:local

FROM ${FPS_BASE_IMAGE}

RUN set -eux; \
    if command -v apt-get >/dev/null 2>&1; then \
      export DEBIAN_FRONTEND=noninteractive; \
      apt-get update; \
      apt-get install -y --no-install-recommends dante-server; \
      rm -rf /var/lib/apt/lists/*; \
    elif command -v apk >/dev/null 2>&1; then \
      apk add --no-cache dante-server; \
      ln -sf /usr/sbin/sockd /usr/sbin/danted; \
      ln -sf /usr/sbin/sockd /usr/local/bin/danted; \
    else \
      echo "unsupported base image: apt-get or apk is required" >&2; \
      exit 2; \
    fi

COPY examples/docker/proxy-dante/fps-dante-entrypoint.sh /usr/local/bin/fps-dante-entrypoint.sh

RUN chmod 0755 /usr/local/bin/fps-dante-entrypoint.sh

EXPOSE 1080/tcp

ENTRYPOINT ["tini", "--", "/usr/local/bin/fps-dante-entrypoint.sh"]
CMD ["run"]
