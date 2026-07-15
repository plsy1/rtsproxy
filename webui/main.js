'use strict';

class Dashboard {
    constructor() {
        this.statusEndpoint = '/api/status';
        this.logEndpoint = '/api/logs';
        this.statusInFlight = false;
        this.logsInFlight = false;
        this.logsPaused = false;
        this.serverLogs = null;
        this.visibleLogs = [];
        this.historyLength = 60;
        this.upHistory = new Array(this.historyLength).fill(0);
        this.downHistory = new Array(this.historyLength).fill(0);

        this.elements = {
            brandHome: document.getElementById('brand-home'),
            serverStatus: document.getElementById('server-status'),
            serverStatusText: document.getElementById('server-status-text'),
            lastUpdated: document.getElementById('last-updated'),
            refreshButton: document.getElementById('refresh-now'),
            upBandwidth: document.getElementById('up-bandwidth'),
            downBandwidth: document.getElementById('down-bandwidth'),
            upTraffic: document.getElementById('up-traffic'),
            downTraffic: document.getElementById('down-traffic'),
            activeClients: document.getElementById('active-clients'),
            bufferUsage: document.getElementById('buffer-usage'),
            bufferSummary: document.getElementById('buffer-summary'),
            bufferProgress: document.getElementById('buffer-progress'),
            gauge: document.getElementById('buffer-gauge'),
            gaugeValue: document.getElementById('gauge-value'),
            poolHealth: document.getElementById('pool-health'),
            usedBuffers: document.getElementById('used-count'),
            availableBuffers: document.getElementById('available-count'),
            peakBuffers: document.getElementById('peak-count'),
            totalMemory: document.getElementById('total-memory'),
            chartUpValue: document.getElementById('chart-up-value'),
            chartDownValue: document.getElementById('chart-down-value'),
            chartMaxValue: document.getElementById('chart-max-value'),
            chartUpLine: document.getElementById('chart-up-line'),
            chartDownLine: document.getElementById('chart-down-line'),
            chartUpArea: document.getElementById('chart-up-area'),
            chartDownArea: document.getElementById('chart-down-area'),
            sessionsBody: document.getElementById('sessions-body'),
            sessionCount: document.getElementById('session-count'),
            logDisplay: document.getElementById('log-display'),
            logSearch: document.getElementById('log-search'),
            logLevelSelect: document.getElementById('log-level-select'),
            pauseLogsButton: document.getElementById('pause-logs'),
            clearLogsButton: document.getElementById('clear-logs')
        };

        this.init();
    }

    init() {
        if (this.elements.brandHome) {
            this.elements.brandHome.href = this.endpointUrl('/admin/');
        }

        const savedLevel = localStorage.getItem('rtsproxy.logLevel');
        if (savedLevel !== null && this.elements.logLevelSelect) {
            this.elements.logLevelSelect.value = savedLevel;
        }

        this.elements.refreshButton?.addEventListener('click', () => {
            this.refresh();
            if (!this.logsPaused) this.fetchLogs();
        });

        this.elements.logLevelSelect?.addEventListener('change', () => {
            localStorage.setItem('rtsproxy.logLevel', this.elements.logLevelSelect.value);
            this.renderLogs();
        });

        this.elements.logSearch?.addEventListener('input', () => this.renderLogs());
        this.elements.pauseLogsButton?.addEventListener('click', () => this.toggleLogs());
        this.elements.clearLogsButton?.addEventListener('click', () => {
            this.visibleLogs = [];
            this.renderLogs();
        });

        document.addEventListener('visibilitychange', () => {
            if (!document.hidden) {
                this.refresh();
                if (!this.logsPaused) this.fetchLogs();
            }
        });

        this.renderEmptySessions();
        this.renderLogs();
        this.updateChart();
        this.refresh();
        this.fetchLogs();

        window.setInterval(() => {
            if (!document.hidden) this.refresh();
        }, 1000);
        window.setInterval(() => {
            if (!document.hidden && !this.logsPaused) this.fetchLogs();
        }, 750);
    }

    endpointUrl(endpoint) {
        const url = new URL(endpoint, window.location.origin);
        const token = new URLSearchParams(window.location.search).get('token');
        if (token) url.searchParams.set('token', token);
        return url.toString();
    }

    async fetchJson(endpoint, timeoutMs = 4500) {
        const controller = new AbortController();
        const timeout = window.setTimeout(() => controller.abort(), timeoutMs);
        try {
            const response = await fetch(this.endpointUrl(endpoint), {
                cache: 'no-store',
                headers: { Accept: 'application/json' },
                signal: controller.signal
            });
            if (!response.ok) throw new Error(`HTTP ${response.status}`);
            return await response.json();
        } finally {
            window.clearTimeout(timeout);
        }
    }

    async refresh() {
        if (this.statusInFlight) return;
        this.statusInFlight = true;
        this.elements.refreshButton?.classList.add('loading');

        try {
            const data = await this.fetchJson(this.statusEndpoint);
            this.updateStatus(data);
            this.updateSessions(Array.isArray(data.clients) ? data.clients : []);
            this.setConnectionState('online');
        } catch (error) {
            this.setConnectionState('offline');
        } finally {
            this.statusInFlight = false;
            this.elements.refreshButton?.classList.remove('loading');
        }
    }

    setConnectionState(state) {
        if (!this.elements.serverStatus || !this.elements.serverStatusText) return;
        this.elements.serverStatus.classList.remove('online', 'offline', 'connecting');
        this.elements.serverStatus.classList.add(state);

        if (state === 'online') {
            this.elements.serverStatusText.textContent = '运行中';
            this.elements.lastUpdated.textContent = new Date().toLocaleTimeString('zh-CN', {
                hour12: false,
                hour: '2-digit',
                minute: '2-digit',
                second: '2-digit'
            });
        } else {
            this.elements.serverStatusText.textContent = '已离线';
            this.elements.lastUpdated.textContent = '连接中断';
        }
    }

    updateStatus(data) {
        const stats = data?.stats && typeof data.stats === 'object' ? data.stats : {};
        const pool = data?.pool && typeof data.pool === 'object' ? data.pool : {};
        const upBitsPerSecond = this.toNumber(stats.up_bandwidth) * 8;
        const downBitsPerSecond = this.toNumber(stats.down_bandwidth) * 8;

        this.setText(this.elements.upBandwidth, this.formatBitRate(upBitsPerSecond));
        this.setText(this.elements.downBandwidth, this.formatBitRate(downBitsPerSecond));
        this.setText(this.elements.chartUpValue, this.formatBitRate(upBitsPerSecond));
        this.setText(this.elements.chartDownValue, this.formatBitRate(downBitsPerSecond));
        this.setText(this.elements.upTraffic, this.formatBytes(this.toNumber(stats.up_traffic)));
        this.setText(this.elements.downTraffic, this.formatBytes(this.toNumber(stats.down_traffic)));
        this.setText(this.elements.activeClients, this.formatInteger(stats.active_clients));

        const allocated = Math.max(0, this.toNumber(pool.allocated));
        const reportedUsed = Math.max(0, this.toNumber(pool.used));
        const used = allocated > 0 ? Math.min(reportedUsed, allocated) : reportedUsed;
        const available = Math.max(0, this.toNumber(pool.available));
        const peak = Math.max(0, this.toNumber(pool.peak));
        const usage = allocated > 0 ? Math.min(100, (used / allocated) * 100) : 0;

        this.setText(this.elements.bufferUsage, `${usage.toFixed(1)}%`);
        this.setText(this.elements.bufferSummary, `${this.formatInteger(used)} / ${this.formatInteger(allocated)} buffers`);
        if (this.elements.bufferProgress) this.elements.bufferProgress.style.width = `${usage}%`;
        this.setText(this.elements.gaugeValue, `${Math.round(usage)}%`);
        if (this.elements.gauge) {
            this.elements.gauge.style.setProperty('--gauge-value', `${usage * 3.6}deg`);
            this.elements.gauge.setAttribute('aria-label', `缓冲池占用 ${usage.toFixed(1)}%`);
        }
        this.setText(this.elements.usedBuffers, this.formatInteger(used));
        this.setText(this.elements.availableBuffers, this.formatInteger(available));
        this.setText(this.elements.peakBuffers, this.formatInteger(peak));
        this.setText(this.elements.totalMemory, this.formatBytes(this.toNumber(pool.total_bytes)));
        this.updatePoolHealth(usage);

        this.upHistory.shift();
        this.upHistory.push(upBitsPerSecond);
        this.downHistory.shift();
        this.downHistory.push(downBitsPerSecond);
        this.updateChart();
    }

    updatePoolHealth(usage) {
        if (!this.elements.poolHealth) return;
        this.elements.poolHealth.classList.remove('warning', 'critical');
        if (usage >= 85) {
            this.elements.poolHealth.textContent = '压力较高';
            this.elements.poolHealth.classList.add('critical');
        } else if (usage >= 65) {
            this.elements.poolHealth.textContent = '需要关注';
            this.elements.poolHealth.classList.add('warning');
        } else {
            this.elements.poolHealth.textContent = '健康';
        }
    }

    updateChart() {
        const observedMax = Math.max(...this.upHistory, ...this.downHistory, 0);
        const scaleMax = this.niceScale(observedMax);
        const upPoints = this.chartPoints(this.upHistory, scaleMax);
        const downPoints = this.chartPoints(this.downHistory, scaleMax);

        this.elements.chartUpLine?.setAttribute('points', upPoints);
        this.elements.chartDownLine?.setAttribute('points', downPoints);
        this.elements.chartUpArea?.setAttribute('d', this.areaPath(upPoints));
        this.elements.chartDownArea?.setAttribute('d', this.areaPath(downPoints));
        this.setText(this.elements.chartMaxValue, this.formatBitRate(scaleMax));
    }

    chartPoints(values, scaleMax) {
        const width = 600;
        const baseline = 167.5;
        const chartHeight = 157.5;
        const lastIndex = Math.max(1, values.length - 1);
        return values.map((value, index) => {
            const x = (index / lastIndex) * width;
            const safeValue = Math.max(0, this.toNumber(value));
            const y = baseline - Math.min(1, safeValue / scaleMax) * chartHeight;
            return `${x.toFixed(2)},${y.toFixed(2)}`;
        }).join(' ');
    }

    areaPath(points) {
        if (!points) return '';
        const firstX = points.split(' ', 1)[0].split(',')[0];
        const lastPoint = points.slice(points.lastIndexOf(' ') + 1);
        const lastX = lastPoint.split(',')[0];
        return `M ${firstX} 167.5 L ${points.split(' ').join(' L ')} L ${lastX} 167.5 Z`;
    }

    niceScale(value) {
        if (!Number.isFinite(value) || value <= 0) return 1000000;
        const exponent = Math.pow(10, Math.floor(Math.log10(value)));
        const fraction = value / exponent;
        const ceiling = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
        return Math.max(1000, ceiling * exponent);
    }

    updateSessions(clients) {
        if (!this.elements.sessionsBody) return;
        this.elements.sessionsBody.replaceChildren();
        this.setText(this.elements.sessionCount, this.formatInteger(clients.length));

        if (clients.length === 0) {
            this.renderEmptySessions();
            return;
        }

        const fragment = document.createDocumentFragment();
        clients.forEach((client) => fragment.appendChild(this.createSessionRow(client || {})));
        this.elements.sessionsBody.appendChild(fragment);
    }

    createSessionRow(client) {
        const row = document.createElement('tr');
        row.appendChild(this.createEndpointCell(client.downstream, 'Downstream'));
        row.appendChild(this.createEndpointCell(client.upstream, 'Upstream'));

        const typeCell = document.createElement('td');
        const mode = document.createElement('span');
        const isMitm = String(client.type || '').toLowerCase() === 'mitm';
        mode.className = `mode-tag${isMitm ? ' mitm' : ''}`;
        mode.textContent = isMitm ? 'RTSP MITM' : 'HTTP TUNNEL';
        typeCell.appendChild(mode);
        row.appendChild(typeCell);

        const rateCell = document.createElement('td');
        rateCell.className = 'rate-cell';
        rateCell.appendChild(this.createRateLine(client.upstream_bandwidth, false));
        rateCell.appendChild(this.createRateLine(client.downstream_bandwidth, true));
        row.appendChild(rateCell);

        const transportCell = document.createElement('td');
        const transport = String(client.transport || 'UDP').toUpperCase();
        const transportTag = document.createElement('span');
        const tagClass = transport.includes('INTERLEAVED') ? 'interleaved' : transport.includes('TCP') ? 'tcp' : 'udp';
        transportTag.className = `transport-tag ${tagClass}`;
        transportTag.textContent = transport;
        transportCell.appendChild(transportTag);
        row.appendChild(transportCell);

        const durationCell = document.createElement('td');
        durationCell.textContent = this.formatDuration(client.proxy);
        row.appendChild(durationCell);
        return row;
    }

    createEndpointCell(value, label) {
        const cell = document.createElement('td');
        const wrapper = document.createElement('div');
        const primary = document.createElement('span');
        const secondary = document.createElement('span');
        wrapper.className = 'endpoint-cell';
        primary.className = 'endpoint-primary';
        secondary.className = 'endpoint-secondary';
        primary.textContent = String(value || 'Connecting...');
        primary.title = primary.textContent;
        secondary.textContent = label;
        wrapper.append(primary, secondary);
        cell.appendChild(wrapper);
        return cell;
    }

    createRateLine(value, isDownstream) {
        const line = document.createElement('span');
        line.className = `rate-line${isDownstream ? ' down' : ''}`;
        line.textContent = this.formatBitRate(this.toNumber(value));
        return line;
    }

    renderEmptySessions() {
        if (!this.elements.sessionsBody) return;
        this.elements.sessionsBody.replaceChildren();

        const row = document.createElement('tr');
        const cell = document.createElement('td');
        const state = document.createElement('div');
        const icon = document.createElement('span');
        const title = document.createElement('strong');
        const description = document.createElement('span');

        row.className = 'empty-row';
        cell.colSpan = 6;
        state.className = 'empty-state';
        icon.className = 'empty-state-icon';
        icon.textContent = '—';
        title.textContent = '暂无活动会话';
        description.textContent = '新的代理连接会自动显示在这里';
        state.append(icon, title, description);
        cell.appendChild(state);
        row.appendChild(cell);
        this.elements.sessionsBody.appendChild(row);
    }

    async fetchLogs() {
        if (this.logsInFlight || this.logsPaused) return;
        this.logsInFlight = true;
        try {
            const data = await this.fetchJson(this.logEndpoint);
            this.mergeLogs(Array.isArray(data.logs) ? data.logs : []);
        } catch (error) {
            // Status polling owns the visible connection indicator.
        } finally {
            this.logsInFlight = false;
        }
    }

    mergeLogs(rawLogs) {
        const incoming = rawLogs.map((line) => String(line));
        if (this.serverLogs === null) {
            this.serverLogs = incoming;
            this.visibleLogs = incoming.slice(-500);
            this.renderLogs(true);
            return;
        }

        const overlap = this.findLogOverlap(this.serverLogs, incoming);
        const additions = incoming.slice(overlap);
        this.serverLogs = incoming;
        if (additions.length === 0) return;

        this.visibleLogs.push(...additions);
        if (this.visibleLogs.length > 500) {
            this.visibleLogs.splice(0, this.visibleLogs.length - 500);
        }
        this.renderLogs(true);
    }

    findLogOverlap(previous, next) {
        const max = Math.min(previous.length, next.length);
        for (let length = max; length > 0; length -= 1) {
            let matches = true;
            const previousStart = previous.length - length;
            for (let index = 0; index < length; index += 1) {
                if (previous[previousStart + index] !== next[index]) {
                    matches = false;
                    break;
                }
            }
            if (matches) return length;
        }
        return 0;
    }

    renderLogs(newData = false) {
        if (!this.elements.logDisplay) return;
        const selectedLevel = Number.parseInt(this.elements.logLevelSelect?.value || '1', 10);
        const query = (this.elements.logSearch?.value || '').trim().toLocaleLowerCase();
        const filtered = this.visibleLogs.filter((line) => {
            return this.logLevel(line) >= selectedLevel && (!query || line.toLocaleLowerCase().includes(query));
        });
        const wasAtBottom = this.isLogViewAtBottom();
        this.elements.logDisplay.replaceChildren();

        if (filtered.length === 0) {
            const empty = document.createElement('div');
            empty.className = 'log-empty';
            empty.textContent = this.logsPaused ? '日志流已暂停' : query ? '没有匹配的日志' : '等待新的日志事件';
            this.elements.logDisplay.appendChild(empty);
            return;
        }

        const fragment = document.createDocumentFragment();
        filtered.forEach((line) => {
            const levelName = this.logLevelName(line);
            const entry = document.createElement('div');
            const mark = document.createElement('span');
            const content = document.createElement('span');
            entry.className = `log-entry ${levelName}`;
            mark.className = 'log-level-mark';
            content.className = 'log-text';
            content.textContent = line;
            entry.append(mark, content);
            fragment.appendChild(entry);
        });
        this.elements.logDisplay.appendChild(fragment);

        if (newData && wasAtBottom) {
            this.elements.logDisplay.scrollTop = this.elements.logDisplay.scrollHeight;
        }
    }

    isLogViewAtBottom() {
        const view = this.elements.logDisplay;
        if (!view || view.scrollHeight <= view.clientHeight) return true;
        return view.scrollHeight - view.scrollTop - view.clientHeight < 60;
    }

    logLevel(line) {
        if (line.includes('[ERROR]')) return 3;
        if (line.includes('[WARN]')) return 2;
        if (line.includes('[DEBUG]')) return 0;
        return 1;
    }

    logLevelName(line) {
        const level = this.logLevel(line);
        return level === 3 ? 'error' : level === 2 ? 'warn' : level === 0 ? 'debug' : 'info';
    }

    toggleLogs() {
        this.logsPaused = !this.logsPaused;
        if (this.elements.pauseLogsButton) {
            this.elements.pauseLogsButton.textContent = this.logsPaused ? '继续' : '暂停';
            this.elements.pauseLogsButton.classList.toggle('active', this.logsPaused);
            this.elements.pauseLogsButton.setAttribute('aria-pressed', String(this.logsPaused));
        }
        this.renderLogs();
        if (!this.logsPaused) this.fetchLogs();
    }

    setText(element, text) {
        if (element) element.textContent = String(text);
    }

    toNumber(value) {
        const number = Number(value);
        return Number.isFinite(number) ? number : 0;
    }

    formatInteger(value) {
        return Math.max(0, Math.trunc(this.toNumber(value))).toLocaleString('zh-CN');
    }

    formatBytes(value) {
        const bytes = Math.max(0, this.toNumber(value));
        if (bytes === 0) return '0 B';
        const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
        const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
        const amount = bytes / Math.pow(1024, index);
        return `${this.compactNumber(amount)} ${units[index]}`;
    }

    formatBitRate(value) {
        const bits = Math.max(0, this.toNumber(value));
        const units = ['bps', 'Kbps', 'Mbps', 'Gbps', 'Tbps'];
        if (bits === 0) return '0 bps';
        const index = Math.min(Math.floor(Math.log(bits) / Math.log(1000)), units.length - 1);
        const amount = bits / Math.pow(1000, index);
        return `${this.compactNumber(amount)} ${units[index]}`;
    }

    compactNumber(value) {
        if (value >= 100) return value.toFixed(0);
        if (value >= 10) return value.toFixed(1).replace(/\.0$/, '');
        return value.toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
    }

    formatDuration(rawSeconds) {
        const seconds = Math.max(0, Math.trunc(this.toNumber(rawSeconds)));
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        const remaining = seconds % 60;
        return [hours, minutes, remaining].map((part) => String(part).padStart(2, '0')).join(':');
    }
}

document.addEventListener('DOMContentLoaded', () => new Dashboard());
