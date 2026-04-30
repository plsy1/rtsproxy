/**
 * RTProxy Memory Pool Dashboard
 * 2x2 Professional Layout Logic - Stable Version
 */

interface PoolStats {
    available: number;
    allocated: number;
    used: number;
    peak: number;
    buffer_size: number;
    total_bytes: number;
}

interface SystemStats {
    traffic: number;
    bandwidth: number;
    active_clients: number;
}

interface ClientInfo {
    type: string;
    transport: string;
    upstream: string;
    proxy: string;
    downstream: string;
}

interface StatusResponse {
    pool: PoolStats;
    stats: SystemStats;
    clients: ClientInfo[];
}

interface LogResponse {
    logs: string[];
    level: number;
}

class MemoryPoolMonitor {
    private apiEndpoint = '/api/status';
    private logEndpoint = '/api/logs';
    private levelEndpoint = '/api/loglevel';
    private refreshInterval = 500;
    
    private canvas: HTMLCanvasElement;
    private ctx: CanvasRenderingContext2D;
    private statusBadge: HTMLElement | null;
    private lastUpdateEl: HTMLElement | null;
    private sessionsBody: HTMLElement | null;
    private logDisplay: HTMLElement | null;
    private logLevelSelect: HTMLSelectElement | null;
    private clearLogsBtn: HTMLButtonElement | null;
    
    private elements: { [key: string]: HTMLElement | null } = {};

    private colors = {
        used: '#ff4d4d',
        free: '#2a2a35'
    };

    private lastLogTimestamp = "";

    constructor() {
        this.canvas = document.getElementById('buffer-canvas') as HTMLCanvasElement;
        this.ctx = this.canvas ? this.canvas.getContext('2d')! : null as any;
        this.statusBadge = document.getElementById('server-status');
        this.lastUpdateEl = document.getElementById('last-update');
        this.sessionsBody = document.getElementById('sessions-body');
        this.logDisplay = document.getElementById('log-display');
        this.logLevelSelect = document.getElementById('log-level-select') as HTMLSelectElement;
        this.clearLogsBtn = document.getElementById('clear-logs') as HTMLButtonElement;
        
        // Map all potential display elements
        const ids = [
            'available-count', 'used-count', 'used-percent-short', 
            'total-memory', 'bandwidth', 'active-clients', 'total-traffic'
        ];
        ids.forEach(id => {
            this.elements[id] = document.getElementById(id);
        });

        const style = getComputedStyle(document.documentElement);
        this.colors.used = style.getPropertyValue('--used-color').trim() || '#ff4d4d';
        this.colors.free = style.getPropertyValue('--free-color').trim() || '#2a2a35';

        if (this.canvas) {
            window.addEventListener('resize', () => this.syncCanvasSize());
            this.syncCanvasSize();
        }
        this.init();
    }

    private syncCanvasSize() {
        if (!this.canvas) return;
        const rect = this.canvas.getBoundingClientRect();
        if (this.canvas.width !== rect.width || this.canvas.height !== rect.height) {
            this.canvas.width = rect.width;
            this.canvas.height = rect.height;
        }
    }

    private formatBytes(bytes: number, decimals = 2) {
        if (bytes === 0) return '0 B';
        const k = 1024;
        const dm = decimals < 0 ? 0 : decimals;
        const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
        const i = Math.floor(Math.log(bytes) / Math.log(k));
        return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
    }

    private async init() {
        if (this.logLevelSelect) {
            this.logLevelSelect.addEventListener('change', () => this.changeLogLevel());
        }
        if (this.clearLogsBtn && this.logDisplay) {
            this.clearLogsBtn.addEventListener('click', () => { this.logDisplay!.innerHTML = ''; });
        }
        this.startPolling();
    }

    private startPolling() {
        setInterval(() => {
            this.fetchStatus();
            this.fetchLogs();
        }, this.refreshInterval);
    }

    private async changeLogLevel() {
        if (!this.logLevelSelect) return;
        const level = this.logLevelSelect.value;
        try {
            await fetch(`${this.levelEndpoint}?level=${level}`);
        } catch (e) {}
    }

    private async fetchStatus() {
        try {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.apiEndpoint}?token=${token}` : this.apiEndpoint;
            const response = await fetch(fetchUrl);
            if (!response.ok) throw new Error('Offline');
            const data: StatusResponse = await response.json();
            this.updateUI(data);
            this.setOnline(true);
        } catch (error) {
            this.setOnline(false);
        }
    }

    private async fetchLogs() {
        try {
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.logEndpoint}?token=${token}` : this.logEndpoint;
            const response = await fetch(fetchUrl);
            if (!response.ok) return;
            const data: LogResponse = await response.json();
            this.updateLogs(data);
        } catch (e) {}
    }

    private updateLogs(data: LogResponse) {
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

    private setOnline(isOnline: boolean) {
        if (!this.statusBadge) return;
        if (isOnline) {
            this.statusBadge.classList.remove('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> ONLINE';
        } else {
            this.statusBadge.classList.add('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> OFFLINE';
        }
    }

    private updateUI(data: StatusResponse) {
        const { available, allocated, used, total_bytes } = data.pool;
        const { traffic, bandwidth, active_clients } = data.stats;

        if (this.elements['available-count']) this.elements['available-count'].innerText = available.toLocaleString();
        if (this.elements['used-count']) this.elements['used-count'].innerText = used.toLocaleString();
        
        const occupancy = allocated > 0 ? (used / allocated * 100).toFixed(1) : '0';
        if (this.elements['used-percent-short']) this.elements['used-percent-short'].innerText = `${occupancy}%`;
        
        const mb = (total_bytes / (1024 * 1024)).toFixed(1);
        if (this.elements['total-memory']) this.elements['total-memory'].innerText = `${mb} MB`;
        
        const bwMbps = (bandwidth * 8 / (1000 * 1000)).toFixed(2);
        if (this.elements['bandwidth']) this.elements['bandwidth'].innerText = `${bwMbps} Mbps`;
        
        if (this.elements['active-clients']) this.elements['active-clients'].innerText = active_clients.toLocaleString();
        if (this.elements['total-traffic']) this.elements['total-traffic'].innerText = this.formatBytes(traffic);
        
        if (this.lastUpdateEl) { this.lastUpdateEl.innerText = new Date().toLocaleTimeString(); }
        
        this.drawMap(allocated, used);
        this.updateSessions(data.clients || []);
    }

    private updateSessions(clients: ClientInfo[]) {
        if (!this.sessionsBody) return;
        this.sessionsBody.innerHTML = clients.map(client => `
            <tr>
                <td>${client.downstream.split(':')[0]}</td>
                <td>Port:${client.proxy.split(':')[1] || client.proxy.split(':')[0]}</td>
                <td>${client.upstream.split(':')[0]}</td>
                <td>${client.type}</td>
            </tr>
        `).join('');
    }

    private drawMap(total: number, used: number) {
        if (!this.canvas || total <= 0) return;
        this.syncCanvasSize();
        const width = this.canvas.width;
        const height = this.canvas.height;
        this.ctx.clearRect(0, 0, width, height);

        let step = Math.floor(Math.sqrt((width * height) / total));
        if (step < 2) step = 2;
        let cols = Math.floor(width / step);
        let rows = Math.ceil(total / cols);
        if (rows * step > height) {
            step = Math.max(1, Math.floor(height / Math.ceil(total / (width / step))));
            cols = Math.floor(width / step);
        }

        const gap = step > 3 ? 1 : 0.5;
        const boxSize = step - gap;
        const displayUsed = Math.floor((used / total) * total);

        for (let i = 0; i < total; i++) {
            const x = (i % cols) * step;
            const y = Math.floor(i / cols) * step;
            if (y > height) break;
            this.ctx.fillStyle = i < displayUsed ? this.colors.used : this.colors.free;
            this.ctx.fillRect(x, y, boxSize, boxSize);
        }
    }
}

document.addEventListener('DOMContentLoaded', () => { new MemoryPoolMonitor(); });
