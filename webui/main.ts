/**
 * RTProxy Memory Pool Dashboard
 * Pure TypeScript Source
 */

interface PoolStats {
    available: number;
    allocated: number;
    used: number;
    peak: number;
    buffer_size: number;
    total_bytes: number;
}

interface StatusResponse {
    pool: PoolStats;
}

class MemoryPoolMonitor {
    private apiEndpoint = '/api/status';
    private refreshInterval = 500;
    private gridElement: HTMLElement;
    private statusBadge: HTMLElement;
    private lastUpdateEl: HTMLElement | null;
    
    private elements: {
        available: HTMLElement;
        used: HTMLElement;
        usedPercent: HTMLElement;
        total: HTMLElement;
        memory: HTMLElement;
        bufSize: HTMLElement;
    };

    constructor() {
        this.gridElement = document.getElementById('buffer-grid')!;
        this.statusBadge = document.getElementById('server-status')!;
        this.lastUpdateEl = document.getElementById('last-update')!;
        
        this.elements = {
            available: document.getElementById('available-count')!,
            used: document.getElementById('used-count')!,
            usedPercent: document.getElementById('used-percent')!,
            total: document.getElementById('total-count')!,
            memory: document.getElementById('total-memory')!,
            bufSize: document.getElementById('buffer-size-info')!
        };

        this.init();
    }

    private async init() {
        this.startPolling();
    }

    private startPolling() {
        this.fetchStatus();
        setInterval(() => this.fetchStatus(), this.refreshInterval);
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
            console.error('Fetch error:', error);
            this.setOnline(false);
        }
    }

    private setOnline(isOnline: boolean) {
        if (isOnline) {
            this.statusBadge.classList.remove('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> ONLINE';
        } else {
            this.statusBadge.classList.add('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> OFFLINE';
        }
    }

    private updateUI(data: StatusResponse) {
        const { available, allocated, used, buffer_size, total_bytes } = data.pool;
        
        this.elements.available.innerText = available.toLocaleString();
        this.elements.used.innerText = used.toLocaleString();
        this.elements.total.innerText = allocated.toLocaleString();
        
        const occupancy = allocated > 0 ? (used / allocated * 100).toFixed(1) : '0';
        this.elements.usedPercent.innerText = `${occupancy}% occupancy (Peak: ${data.pool.peak})`;
        
        const mb = (total_bytes / (1024 * 1024)).toFixed(1);
        this.elements.memory.innerText = `${mb} MB`;
        this.elements.bufSize.innerText = `Size: ${(buffer_size / 1024).toFixed(0)} KB each`;

        if (this.lastUpdateEl) {
            this.lastUpdateEl.innerText = new Date().toLocaleTimeString();
        }

        this.updateGrid(allocated, used);
    }

    private updateGrid(total: number, used: number) {
        const currentBlocks = this.gridElement.children.length;
        const displayTotal = Math.min(total, 10000); 
        const displayUsed = Math.floor((used / total) * displayTotal);

        if (currentBlocks !== displayTotal) {
            this.gridElement.innerHTML = '';
            for (let i = 0; i < displayTotal; i++) {
                const block = document.createElement('div');
                block.className = 'buffer-block';
                this.gridElement.appendChild(block);
            }
        }

        const blocks = this.gridElement.children;
        for (let i = 0; i < blocks.length; i++) {
            if (i < displayUsed) {
                blocks[i].classList.add('used');
            } else {
                blocks[i].classList.remove('used');
            }
        }
    }
}

document.addEventListener('DOMContentLoaded', () => {
    new MemoryPoolMonitor();
});
