# TLS Certificates for j9t Web Server

j9t serves its dashboard and workflow editor over HTTPS using TLS certificates
configured in `config.json`:

```json
"TlsCert": "certs/j9t-cert.pem",
"TlsKey": "certs/j9t-key.pem"
```

## Generating a Self-Signed Certificate

Use OpenSSL to create a private key and self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout certs/j9t-key.pem \
  -out certs/j9t-cert.pem \
  -days 365 \
  -subj "/CN=localhost"
```

| Flag | Purpose |
|------|---------|
| `-x509` | Generate a self-signed certificate (not a CSR) |
| `-newkey rsa:2048` | Create a new 2048-bit RSA key |
| `-nodes` | No passphrase on the private key |
| `-days 365` | Certificate validity period |
| `-subj "/CN=localhost"` | Common Name — change to your hostname or IP if needed |

### Adding Subject Alternative Names

Browsers require SAN entries for local development. To include additional
hostnames or IPs:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout certs/j9t-key.pem \
  -out certs/j9t-cert.pem \
  -days 365 \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
```

## File Permissions

The private key should be readable only by the owner:

```bash
chmod 600 certs/j9t-key.pem
```

## Git

Certificate files are excluded from version control via `.gitignore` in this
directory. Each developer or deployment must generate its own certificates.
