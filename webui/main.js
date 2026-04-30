class MemoryPoolMonitor {
    constructor() {
        this.apiEndpoint = '/api/status';
        this.logEndpoint = '/api/logs';
        this.levelEndpoint = '/api/loglevel';
        this.refreshInterval = 500;
        
        this.canvas = document.getElementById('buffer-canvas');
        this.ctx = this.canvas ? this.canvas.getContext('2d') : null;
        this.statusBadge = document.getElementById('server-status');
        this.sessionsBody = document.getElementById('sessions-body');
        this.logDisplay = document.getElementById('log-display');
        this.logLevelSelect = document.getElementById('log-level-select');
        this.clearLogsBtn = document.getElementById('clear-logs');
        
        this.elements = {};
        const ids = [
            'available-count', 'used-count', 'total-buffers', 
            'total-memory', 'bandwidth', 'active-clients', 'total-traffic'
        ];
        ids.forEach(id => {
            this.elements[id] = document.getElementById(id);
        });

        this.colors = {
            used: '#ff4d4d',
            free: '#2a2a35'
        };

        this.lastLogTimestamp = "";

        const style = getComputedStyle(document.documentElement);
        this.colors.used = style.getPropertyValue('--used-color').trim() || '#ff4d4d';
        this.colors.free = style.getPropertyValue('--free-color').trim() || '#2a2a35';

        if (this.canvas) {
            window.addEventListener('resize', () => this.syncCanvasSize());
            this.syncCanvasSize();
        }
        this.init();
    }

    syncCanvasSize() {
        if (!this.canvas) return;
        const rect = this.canvas.getBoundingClientRect();
        if (this.canvas.width !== rect.width || this.canvas.height !== rect.height) {
            this.canvas.width = rect.width;
            this.canvas.height = rect.height;
        }
    }

    formatBytes(bytes, decimals = 2) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const dm = decimals < 0 ? 0 : decimals;
        const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
    }

    formatDuration(seconds) {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        const s = Math.floor(seconds % 60);
        return [h, m, s].map(v => v.toString().padStart(2, '0')).join(':');
    }

    async init() {
        if (this.logLevelSelect) {
            this.logLevelSelect.addEventListener('change', () => this.changeLogLevel());
        }
        if (this.clearLogsBtn && this.logDisplay) {
            this.clearLogsBtn.addEventListener('click', () => { this.logDisplay.innerHTML = ''; });
        }
        this.startPolling();
    }

    startPolling() {
        setInterval(() => {
            this.fetchStatus();
            this.fetchLogs();
        }, this.refreshInterval);
    }

    async changeLogLevel() {
        if (!this.logLevelSelect) return;
        const level = this.logLevelSelect.value;
        try {
            await fetch(`${this.levelEndpoint}?level=${level}`);
        } catch (e) {}
    }

    async fetchStatus() {
        try {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.apiEndpoint}?token=${token}` : this.apiEndpoint;
            const response = await fetch(fetchUrl);
            if (!response.ok) throw new Error('Offline');
            const data = await response.json();
            this.updateUI(data);
            this.setOnline(true);
        } catch (error) {
            this.setOnline(false);
        }
    }

    async fetchLogs() {
        try {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.logEndpoint}?token=${token}` : this.logEndpoint;
            const response = await fetch(fetchUrl);
            if (!response.ok) return;
            const data = await response.json();
            this.updateLogs(data);
        } catch (e) {}
    }

    updateLogs(data) {
        if (!this.logDisplay) return;
        if (this.logLevelSelect && this.logLevelSelect.value === "") {
            this.logLevelSelect.value = data.level.toString();
        }
        const logs = data.logs;
        if (!logs || logs.length === 0) return;
        const lastEntry = logs[logs.length - 1];
        if (lastEntry === this.lastLogTimestamp) return;
        this.lastLogTimestamp = lastEntry;

        this.logDisplay.innerHTML = logs.map(line => {
            let cls = "info";
            if (line.includes("[ERROR]")) cls = "error";
            else if (line.includes("[WARN]")) cls = "warn";
            else if (line.includes("[DEBUG]")) cls = "debug";
            return `<div class="log-entry ${cls}">${line}</div>`;
        }).join('');
        this.logDisplay.scrollTop = this.logDisplay.scrollHeight;
    }

    setOnline(isOnline) {
        if (!this.statusBadge) return;
        if (isOnline) {
            this.statusBadge.classList.remove('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> ONLINE';
        } else {
            this.statusBadge.classList.add('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> OFFLINE';
        }
    }

    updateUI(data) {
        const { available, allocated, used, total_bytes } = data.pool;
        const { traffic, bandwidth, active_clients } = data.stats;

        if (this.elements['available-count']) this.elements['available-count'].innerText = available.toLocaleString();
        if (this.elements['used-count']) this.elements['used-count'].innerText = used.toLocaleString();
        
        if (this.elements['total-buffers']) this.elements['total-buffers'].innerText = allocated.toLocaleString();
        
        const mb = (total_bytes / (1024 * 1024)).toFixed(1);
        if (this.elements['total-memory']) this.elements['total-memory'].innerText = `${mb} MB`;
        
        const bwMbps = (bandwidth * 8 / (1000 * 1000)).toFixed(2);
        if (this.elements['bandwidth']) this.elements['bandwidth'].innerText = `${bwMbps} Mbps`;
        
        if (this.elements['active-clients']) this.elements['active-clients'].innerText = active_clients.toLocaleString();
        if (this.elements['total-traffic']) this.elements['total-traffic'].innerText = this.formatBytes(traffic);
        
        this.drawMap(allocated, used);
        this.updateSessions(data.clients || []);
    }

    updateSessions(clients) {
        if (!this.sessionsBody) return;
        this.sessionsBody.innerHTML = clients.map(client => {
            const transport = client.transport ? client.transport.toLowerCase() : 'udp';
            let tagClass = 'udp';
            if (transport.includes('tcp')) tagClass = 'tcp';
            if (transport.includes('interleaved')) tagClass = 'interleaved';
            
            const durationSeconds = parseInt(client.proxy);
            const durationDisplay = isNaN(durationSeconds) ? '--' : this.formatDuration(durationSeconds);
            const type = client.type === 'mitm' ? 'MITM' : 'HTTP';
            
            return `
                <tr>
                    <td style="color:var(--accent-blue)">${client.downstream}</td>
                    <td>${client.upstream}</td>
                    <td style="font-weight:700; font-size:0.7rem">${type}</td>
                    <td><span class="tag ${tagClass}">${client.transport || 'UDP'}</span></td>
                    <td>${durationDisplay}</td>
                </tr>
            `;
        }).join('');
    }

    drawMap(total, used) {
        if (!this.canvas || total <= 0) return;
        this.syncCanvasSize();
        const width = this.canvas.width;
        const height = this.canvas.height;
        const ctx = this.ctx;
        ctx.clearRect(0, 0, width, height);

        const percent = (used / total);
        const filledCount = Math.floor(percent * 100);
        
        const rows = 10;
        const cols = 10;
        const gap = 6; // slightly larger gap for "filling" look
        const padding = 15; // smaller padding
        
        // Calculate independent sizes to fill the rectangle completely
        const sizeX = (width - (padding * 2) - (gap * (cols - 1))) / cols;
        const sizeY = (height - (padding * 2) - (gap * (rows - 1))) / rows;
        
        const startX = padding;
        const startY = padding;

        for (let i = 0; i < 100; i++) {
            const r = Math.floor(i / 10);
            const c = i % 10;
            const x = startX + (c * (sizeX + gap));
            const y = startY + (r * (sizeY + gap));
            
            if (i < filledCount) {
                ctx.fillStyle = percent > 0.8 ? '#ef4444' : (percent > 0.5 ? '#fb923c' : '#34d399');
            } else {
                ctx.fillStyle = 'rgba(255, 255, 255, 0.03)';
            }
            
            ctx.beginPath();
            ctx.roundRect(x, y, sizeX, sizeY, 2); 
            ctx.fill();
        }

        // Overlay Percentage
        ctx.fillStyle = '#fff';
        ctx.font = `bold ${Math.max(20, sizeY * 1.2)}px Inter, sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText((percent * 100).toFixed(1) + '%', width / 2, height / 2);
        ctx.shadowBlur = 0;
    }
}

document.addEventListener('DOMContentLoaded', () => { new MemoryPoolMonitor(); });
