# Eco-Series-Kitty — Server Compatibility Matrix

## Client Versions

| Client Version | Min Server Version | Cases Added/Changed | Notes |
|---------------|-------------------|---------------------|-------|
| 0.10.7 | 1.0.0 | 0-78 (base) | 20 bug fixes, PTZ tuning, security fixes |
| 0.11.0 | 1.0.0 | — | CI/CD pipeline, kitty package automation |
| 0.12.0 | 1.1.0 | 80 (credential sync) | Dynamic camera credentials from VMS DB |
| 0.12.1 | 1.1.1 | — | Server-only fix: decrypt password before send |

## Server Repository

[mqtt-server](https://github.com/Rahul-ambiplatforms/mqtt-server)

## Compatibility Rules

1. **NEVER change the JSON format of an existing case number** — only add new cases
2. Client must always be **forward compatible** — unknown cases ignored via `default:`
3. Default credentials (`admin:""`) used when server doesn't send case 80
4. Document minimum server version when adding new cases
