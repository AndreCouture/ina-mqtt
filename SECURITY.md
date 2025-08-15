# Security Policy

## Supported Versions

The following versions of this project are currently supported with security updates:

| Version      | Supported          |
| ------------ | ------------------ |
| `main` branch (latest) | ✅ Yes              |
| Older or tagged versions | ❌ No               |

## Reporting a Vulnerability

If you discover a security vulnerability in this project, please **do not** open a public issue.

Instead, report it privately by sending an email to:

**andre.couture@me.com**

Please include:

- A detailed description of the issue
- Steps to reproduce
- Impact assessment (if available)
- Suggested fixes or mitigations (optional)

We aim to respond to all legitimate vulnerability reports within **5 business days**.

## Security Expectations

This project:

- Does **not** handle sensitive user data directly
- Is **not** intended to be exposed to the internet or run with elevated privileges
- Assumes that your system’s I²C and MQTT interfaces are already securely configured

## Dependencies

This project relies on:

- [Eclipse Paho MQTT C](https://github.com/eclipse/paho.mqtt.c)
- Linux I²C interfaces (via wiringPi or direct IO)
- Optional use of system services (e.g., cron, systemd)

Please ensure your operating system and build environment are up to date and patched against known vulnerabilities.

## Security Best Practices

If you deploy this code in production or a sensitive environment:

- Run it as a **non-root user**
- Use TLS-secured MQTT brokers
- Avoid exposing MQTT topics to untrusted sources
- Secure `.conf` files and MQTT credentials with proper permissions

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for more information.
