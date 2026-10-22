# Reverse engineering environment for re-obiwan.
#
# Contains: Ghidra (headless) + XBE loader, extract-xiso, pyghidra-mcp (Ghidra
# MCP server) and Claude Code. The game ISO is never copied into the image; it
# is bind-mounted from the host repo folder by compose.yaml.

FROM eclipse-temurin:21.0.12.1_1-jdk-noble

ARG GHIDRA_VERSION=12.1.4
ARG GHIDRA_DATE=20260921
ARG GHIDRA_SHA256=ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db

# ghidra-xbe release built against Ghidra 12.1.4
ARG GHIDRA_XBE_TAG=build-202610161023
ARG GHIDRA_XBE_DATE=20261016
ARG GHIDRA_XBE_SHA256=339b2f67563bbb8b5217740319b7f3eae9e9bbacf1f3a8aa29151f7ecb7d5152

# extract-xiso is built from source at a pinned commit (tag build-202609111233)
ARG EXTRACT_XISO_COMMIT=3f5b62cfe68f000b0e3c8a30104973f3a297948e

ARG PYGHIDRA_MCP_VERSION=0.2.7

# Claude Code is installed from npm with a pinned Node.js LTS so the version is
# reproducible; the native installer (claude.ai/install.sh) always tracks latest.
ARG NODE_VERSION=22.23.4
ARG NODE_SHA256=1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce
ARG CLAUDE_CODE_VERSION=2.1.307

ARG USERNAME=re
ARG USER_UID=1000

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
        bash-completion build-essential ca-certificates cmake curl file git jq \
        less nano python3 python3-dev python3-venv ripgrep unzip xxd \
    && rm -rf /var/lib/apt/lists/*

# --- Ghidra -----------------------------------------------------------------
ENV GHIDRA_INSTALL_DIR=/opt/ghidra
RUN curl -fsSL -o /tmp/ghidra.zip \
        "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_${GHIDRA_VERSION}_build/ghidra_${GHIDRA_VERSION}_PUBLIC_${GHIDRA_DATE}.zip" \
    && echo "${GHIDRA_SHA256}  /tmp/ghidra.zip" | sha256sum -c - \
    && unzip -q /tmp/ghidra.zip -d /opt \
    && mv "/opt/ghidra_${GHIDRA_VERSION}_PUBLIC" "${GHIDRA_INSTALL_DIR}" \
    && rm /tmp/ghidra.zip

# --- XBE loader + Jython (ghidra-xbe ships Jython helper scripts) -----------
RUN curl -fsSL -o /tmp/ghidra-xbe.zip \
        "https://github.com/XboxDev/ghidra-xbe/releases/download/${GHIDRA_XBE_TAG}/ghidra_${GHIDRA_VERSION}_PUBLIC_${GHIDRA_XBE_DATE}_ghidra-xbe.zip" \
    && echo "${GHIDRA_XBE_SHA256}  /tmp/ghidra-xbe.zip" | sha256sum -c - \
    && unzip -q /tmp/ghidra-xbe.zip -d "${GHIDRA_INSTALL_DIR}/Ghidra/Extensions" \
    && unzip -q "${GHIDRA_INSTALL_DIR}"/Extensions/Ghidra/*Jython.zip -d "${GHIDRA_INSTALL_DIR}/Ghidra/Extensions" \
    && chmod +x "${GHIDRA_INSTALL_DIR}/Ghidra/Extensions/ghidra-xbe/os/linux_x86_64/XbSymbolDatabaseCLI" \
    && rm /tmp/ghidra-xbe.zip

# --- extract-xiso -------------------------------------------------------------
RUN git init -q /tmp/extract-xiso \
    && git -C /tmp/extract-xiso fetch -q --depth 1 https://github.com/XboxDev/extract-xiso "${EXTRACT_XISO_COMMIT}" \
    && git -C /tmp/extract-xiso checkout -q FETCH_HEAD \
    && cmake -S /tmp/extract-xiso -B /tmp/extract-xiso/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /tmp/extract-xiso/build -j"$(nproc)" \
    && cmake --install /tmp/extract-xiso/build --prefix /usr/local \
    && rm -rf /tmp/extract-xiso

# --- pyghidra-mcp (headless Ghidra MCP server) --------------------------------
ENV VIRTUAL_ENV=/opt/pyghidra-mcp
RUN python3 -m venv "${VIRTUAL_ENV}" \
    && "${VIRTUAL_ENV}/bin/pip" install --no-cache-dir "pyghidra-mcp==${PYGHIDRA_MCP_VERSION}"
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"

# --- Non-root user --------------------------------------------------------------
# The noble base image ships an "ubuntu" user with UID 1000; replace it.
RUN (userdel -r ubuntu 2>/dev/null || true) \
    && useradd -m -s /bin/bash -u "${USER_UID}" "${USERNAME}" \
    && mkdir -p /workspace && chown "${USERNAME}:${USERNAME}" /workspace

COPY bin/ /usr/local/bin/
RUN chmod +x /usr/local/bin/*

# --- Node.js + Claude Code ------------------------------------------------------
RUN curl -fsSL -o /tmp/node.tar.xz \
        "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    && echo "${NODE_SHA256}  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 --no-same-owner \
    && rm /tmp/node.tar.xz /usr/local/CHANGELOG.md /usr/local/LICENSE /usr/local/README.md \
    && npm install -g --no-fund --no-audit "@anthropic-ai/claude-code@${CLAUDE_CODE_VERSION}" \
    && npm cache clean --force \
    && claude --version
USER ${USERNAME}
ENV CLAUDE_CONFIG_DIR=/home/${USERNAME}/.claude \
    DISABLE_AUTOUPDATER=1
RUN mkdir -p "${CLAUDE_CONFIG_DIR}"

ENV WORKSPACE=/workspace \
    GHIDRA_PROJECT_DIR=/workspace/ghidra \
    GHIDRA_PROJECT_NAME=obiwan \
    OBIWAN_ISO=/workspace/obiwan.iso \
    EXTRACT_DIR=/workspace/extracted

WORKDIR /workspace
CMD ["bash"]
