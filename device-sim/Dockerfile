ARG REGISTRY=docker.io
FROM ${REGISTRY}/library/python:3.12-slim
ARG PIP_INDEX_URL=https://pypi.org/simple
RUN pip install --no-cache-dir -i "${PIP_INDEX_URL}" pymodbus==3.7.4 paho-mqtt==2.1.0
WORKDIR /app
COPY sim ./sim
CMD ["python", "-m", "sim"]
