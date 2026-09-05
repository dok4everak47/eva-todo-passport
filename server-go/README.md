# EVA Todo Go Server

This is the self-hosted HTTP deployment of the same `/api/v1` contract used by the Cloudflare Worker. It is intentionally dependency-free for easy Docker deployment. Set `ADMIN_TOKEN` and `DEVICE_TOKEN`, then expose port `8080` privately or through a reverse proxy for public access.

```sh
docker compose up -d --build
```

The current implementation covers health, task listing/creation, completion, reopen, and soft delete. Sync, reports, persistence, and long-poll event parity are next in the compatibility pass before production use.
