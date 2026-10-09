# Single-stage Dockerfile for deploy-manager

FROM python:3.14-slim

# Environment variables
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# Working directory
WORKDIR /app

# Runtime dependencies (Git needed by GitPython)
RUN apt-get update && \
    apt-get install -y --no-install-recommends git && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY services/deploy-manager/pyproject.toml .
COPY services/deploy-manager/src/ ./src/
COPY tests/test_0_code/manifest.yaml /tmp/


# Install deploy-manager and its dependencies
RUN pip install --no-cache-dir .

# Entrypoint
ENTRYPOINT ["python", "-m", "src.main"]