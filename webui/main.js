/**
 * RTProxy Memory Pool Dashboard
 * Pure JS (ES6) - "TS ready" design
 */

class MemoryPoolMonitor {
    constructor() {
        this.apiEndpoint = '/api/status';
        this.refreshInterval = 500;
        this.gridElement = document.getElementById('buffer-grid');
        this.statusBadge = document.getElementById('server-status');
        this.lastUpdateEl = document.getElementById('last-update'); // May be null

        this.elements = {
            available: document.getElementById('available-count'),
            used: document.getElementById('used-count'),
            usedPercent: document.getElementById('used-percent'),
            total: document.getElementById('total-count'),
            memory: document.getElementById('total-memory'),
            bufSize: document.getElementById('buffer-size-info')
        };

        this.init();
    }

    async init() {
        this.startPolling();
    }

    startPolling() {
        this.fetchStatus();
        setInterval(() => this.fetchStatus(), this.refreshInterval);
    }

    async fetchStatus() {
        try {
            // Support token if present in URL
            const urlParams = new URLSearchParams(window.location.search);
            const token = urlParams.get('token');
            const fetchUrl = token ? `${this.apiEndpoint}?token=${token}` : this.apiEndpoint;

            const response = await fetch(fetchUrl);
            if (!response.ok) throw new Error('Offline');

            const data = await response.json();
            this.updateUI(data);
            this.setOnline(true);
        } catch (error) {
            console.error('Fetch error:', error);
            this.setOnline(false);
        }
    }

    setOnline(isOnline) {
        if (isOnline) {
            this.statusBadge.classList.remove('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> ONLINE';
        } else {
            this.statusBadge.classList.add('offline');
            this.statusBadge.innerHTML = '<span class="dot"></span> OFFLINE';
        }
    }

    updateUI(data) {
        const { available, allocated, used, buffer_size, total_bytes } = data.pool;

        this.elements.available.innerText = available.toLocaleString();
        this.elements.used.innerText = used.toLocaleString();
        this.elements.total.innerText = allocated.toLocaleString();

        const occupancy = allocated > 0 ? (used / allocated * 100).toFixed(1) : 0;
        this.elements.usedPercent.innerText = `${occupancy}% occupancy (Peak: ${data.pool.peak})`;

        const mb = (total_bytes / (1024 * 1024)).toFixed(1);
        this.elements.memory.innerText = `${mb} MB`;
        this.elements.bufSize.innerText = `Size: ${(buffer_size / 1024).toFixed(0)} KB each`;

        if (this.lastUpdateEl) {
            this.lastUpdateEl.innerText = new Date().toLocaleTimeString();
        }

        this.updateGrid(allocated, used);
    }

    updateGrid(total, used) {
        // Optimization: only recreate if count changed significantly, 
        // otherwise just update classes.
        const currentBlocks = this.gridElement.children.length;

        // Limit blocks to prevent browser lag if pool is huge
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

// Start the monitor when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    new MemoryPoolMonitor();
});
