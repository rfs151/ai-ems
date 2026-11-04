# 构建上下文是仓库根目录：先编译前端，再放进云端服务一起提供
ARG REGISTRY=docker.io
FROM ${REGISTRY}/library/node:22-alpine AS web
ARG NPM_REGISTRY=https://registry.npmjs.org
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --registry "${NPM_REGISTRY}" --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM ${REGISTRY}/library/python:3.12-slim
ARG PIP_INDEX_URL=https://pypi.org/simple
RUN pip install --no-cache-dir -i "${PIP_INDEX_URL}" fastapi==0.115.6 "uvicorn[standard]==0.34.0" paho-mqtt==2.1.0
WORKDIR /app
COPY cloud/app.py .
COPY --from=web /web/dist ./web
ENV WEB_DIST=/app/web
EXPOSE 8095
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8095", "--proxy-headers"]
