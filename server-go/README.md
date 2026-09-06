# Todo Go Server

This is the self-hosted HTTP deployment of the same `/api/v1` contract used by the Cloudflare Worker. It is dependency-free and stores tasks/reports in the mounted `./data` volume. Set two different tokens: `ADMIN_TOKEN` for the Web UI/AI full access and `DEVICE_TOKEN` for badge sync/report access.

```sh
cp .env.example .env
# edit .env; never commit it
docker compose up -d --build
```

The API listens on `http://localhost:8080/api/v1` by default. Put it behind HTTPS and an authenticated reverse proxy before exposing it publicly. Supported endpoints and request formats are documented in `../docs/API.md`; `/sync`, `/events`, `/report`, `/reports`, CRUD, completion/reopen, filtering, and JSON persistence are implemented.

Open `http://localhost:8080/` for the minimal task web UI. Its `复制 AI SKILL` button copies the generated `/skill.md` document; the same document is downloadable directly from `http://localhost:8080/skill.md`. It describes both `ADMIN_TOKEN` and `DEVICE_TOKEN` without exposing their values.
