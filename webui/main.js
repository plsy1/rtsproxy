class Dashboard {
    constructor() {
        this.statusEndpoint = '/api/status';
        this.logEndpoint = '/api/logs';
        this.lastLogTimestamp = '';

        // DOM Elements
        this.elements = {
            bandwidth: document.getElementById('bandwidth'),
            totalTraffic: document.getElementById('total-traffic'),
            activeClients: document.getElementById('active-clients'),
            usedBuffers: document.getElementById('used-count'),
            totalBuffers: document.getElementById('total-buffers'),
            totalMemory: document.getElementById('total-memory'),
            sessionsBody: document.getElementById('sessions-body'),
            logDisplay: document.getElementById('log-display'),
            logLevelSelect: document.getElementById('log-level-select'),
            clearLogsBtn: document.getElementById('clear-logs'),
            serverStatus: document.getElementById('server-status')
        };

        this.init();
    }

    init() {
        // Load saved log level
        const savedLevel = localStorage.getItem('logLevel');
        if (savedLevel && this.elements.logLevelSelect) {
            this.elements.logLevelSelect.value = savedLevel;
        }

        if (this.elements.clearLogsBtn) {
            this.elements.clearLogsBtn.onclick = () => this.elements.logDisplay.innerHTML = '';
        }
        if (this.elements.logLevelSelect) {
            this.elements.logLevelSelect.onchange = () => this.changeLogLevel();
        }
        
        // Refresh loops
        this.refresh();
        setInterval(() => this.refresh(), 1000);
        setInterval(() => this.fetchLogs(), 500);
    }

    async changeLogLevel() {
        const level = this.elements.logLevelSelect.value;
        localStorage.setItem('logLevel', level);
        // Backend API call removed - filtering is handled locally in updateLogs
    }

    async refresh() {
        try {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.statusEndpoint}?token=${token}` : this.statusEndpoint;
            
            const response = await fetch(fetchUrl);
            if (!response.ok) throw new Error();
            
            const data = await response.json();
            this.updateStatus(data);
            this.updateSessions(data.clients || []);
            
            this.elements.serverStatus.innerHTML = '<span class="dot"></span> ONLINE';
            this.elements.serverStatus.style.color = '#34d399';
        } catch (e) {
            this.elements.serverStatus.innerHTML = '<span class="dot" style="background:#ef4444"></span> OFFLINE';
            this.elements.serverStatus.style.color = '#ef4444';
        }
    }

    updateStatus(data) {
        const stats = data.stats || {};
        const pool = data.pool || {};
        
        const bandwidthMbps = (stats.bandwidth || 0) * 8 / 1000000;
        this.elements.bandwidth.innerText = `${bandwidthMbps.toFixed(2)} Mbps`;
        this.elements.totalTraffic.innerText = this.formatBytes(stats.traffic || 0);
        this.elements.activeClients.innerText = stats.active_clients || 0;
        this.elements.usedBuffers.innerText = `${pool.used || 0} (Peak: ${pool.peak || 0})`;
        this.elements.totalBuffers.innerText = pool.allocated || 0;
        
        const totalMemoryMB = (pool.total_bytes || 0) / (1024 * 1024);
        this.elements.totalMemory.innerText = `${totalMemoryMB.toFixed(1)} MB`;
    }

    updateSessions(clients) {
        if (!this.elements.sessionsBody) return;
        this.elements.sessionsBody.innerHTML = clients.map(client => {
            const transport = client.transport ? client.transport.toLowerCase() : 'udp';
            let tagClass = transport.includes('tcp') ? 'tcp' : (transport.includes('interleaved') ? 'interleaved' : 'udp');
            
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
        if (!this.elements.logDisplay) return;
        const logs = data.logs || [];
        if (logs.length === 0) return;
        
        const lastEntry = logs[logs.length - 1];
        if (lastEntry === this.lastLogTimestamp) return;
        this.lastLogTimestamp = lastEntry;

        const selectedLevel = parseInt(this.elements.logLevelSelect ? this.elements.logLevelSelect.value : "1");
        
        const filteredLogs = logs.filter(line => {
            let lineLevel = 1; 
            if (line.includes("[DEBUG]")) lineLevel = 0;
            else if (line.includes("[INFO]")) lineLevel = 1;
            else if (line.includes("[WARN]")) lineLevel = 2;
            else if (line.includes("[ERROR]")) lineLevel = 3;
            return lineLevel >= selectedLevel;
        });

        this.elements.logDisplay.innerHTML = filteredLogs.map(line => {
            let cls = "info";
            if (line.includes("[ERROR]")) cls = "error";
            else if (line.includes("[WARN]")) cls = "warn";
            else if (line.includes("[DEBUG]")) cls = "debug";
            return `<div class="log-entry ${cls}">${line}</div>`;
        }).join('');
        this.elements.logDisplay.scrollTop = this.elements.logDisplay.scrollHeight;
    }

    formatBytes(bytes) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    }

    formatDuration(seconds) {
        const h = Math.floor(seconds / 3600);
        const m = Math.floor((seconds % 3600) / 60);
        const s = seconds % 60;
        return [h, m, s].map(v => v < 10 ? "0" + v : v).join(":");
    }
}

document.addEventListener('DOMContentLoaded', () => new Dashboard());
