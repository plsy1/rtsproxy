"use strict";
"require form";
"require view";
"require uci";
"require tools.widgets as widgets";

return view.extend({
	load: function() {
		return uci.load('rtsproxy');
	},

	render: function () {
		var m, s, o;

		m = new form.Map('rtsproxy', _('RTSProxy'),
			_('High Performance RTSP Proxy with URL rewriting and NAT traversal.'));

		s = m.section(form.NamedSection, 'main', 'rtsproxy', _('General Settings'));
		s.tab('basic', _('Basic Settings'));
		s.tab('playback', _('Playback Settings'));
		s.tab('nat', _('NAT Settings'));
		s.tab('logging', _('Logging Settings'));

		o = s.taboption('basic', form.Flag, 'enabled', _('Enable'));
		o.rmempty = false;

		var port = uci.get('rtsproxy', 'main', 'port') || '8554';
		var token = uci.get('rtsproxy', 'main', 'auth_token') || '';
		var url = 'http://' + window.location.hostname + ':' + port + '/admin/';
		if (token) url += '?token=' + token;

		o = s.taboption('basic', form.DummyValue, '_webui', _('Management Dashboard'));
		o.rawhtml = true;
		o.default = '<a class="btn cbi-button cbi-button-apply" href="' + url + '" target="_blank" style="margin-top: 5px; display: inline-block;">' + _('Open WebUI') + '</a>';

		o = s.taboption('basic', form.Value, 'port', _('Port'), _('Main listening port (default: 8554)'));
		o.datatype = 'port';
		o.placeholder = '8554';

		o = s.taboption('nat', form.Flag, 'enable_nat', _('Enable NAT'), _('Enable NAT traversal'));

		o = s.taboption('nat', form.ListValue, 'nat_method', _('NAT Method'), _('Select NAT traversal method (default: stun)'));
		o.value('stun', _('STUN Mode'));
		o.value('zte', _('ZTE STB Hole Punching'));
		o.default = 'stun';
		o.depends('enable_nat', '1');

		o = s.taboption('nat', form.Value, 'stun_host', _('STUN Host'), _('STUN server address (default: stun.l.google.com)'));
		o.placeholder = 'stun.l.google.com';
		o.depends({ 'enable_nat': '1', 'nat_method': 'stun' });

		o = s.taboption('nat', form.Value, 'stun_port', _('STUN Port'), _('STUN server port (default: 19302)'));
		o.datatype = 'port';
		o.placeholder = '19302';
		o.depends({ 'enable_nat': '1', 'nat_method': 'stun' });

		o = s.taboption('basic', form.Value, 'buffer_pool_count', _('Buffer Pool Count'), _('Number of blocks in the buffer pool (default: 8192)'));
		o.datatype = 'uinteger';
		o.placeholder = '8192';

		o = s.taboption('basic', form.Value, 'buffer_pool_block_size', _('Buffer Pool Block Size'), _('Size of each block in the buffer pool (default: 2048)'));
		o.datatype = 'uinteger';
		o.placeholder = '2048';

		o = s.taboption('basic', form.Value, 'auth_token', _('Auth Token'), _('Optional token for authentication'));
		o.password = true;

		o = s.taboption('basic', widgets.DeviceSelect, 'listen_interface', _('Listen Interface'), _('Interface to listen on (Downstream)'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.taboption('basic', form.DynamicList, 'blacklist', _('Upstream Blacklist'), _('Blocked IPv4 addresses or CIDR ranges.'));
		o.placeholder = '192.168.0.0/16';
		o.rmempty = true;

		o = s.taboption('basic', form.Flag, 'default_blacklist', _('Default Private-Network Blacklist'), _('Block loopback, RFC1918 and link-local upstreams. Disable this when IPTV servers are on a private network.'));
		o.default = o.enabled;

		// Playback Tab
		o = s.taboption('playback', form.Flag, 'strip_padding', _('Bandwidth Optimization'), _('Strip MPEG-TS Null Packets (PID 0x1FFF) to save bandwidth'));
		o.default = o.disabled;

		o = s.taboption('playback', form.Flag, 'wait_keyframe', _('Startup Optimization'), _('Wait for the first keyframe (I-Frame) to prevent initial green screen'));
		o.default = o.disabled;

		// Logging Tab
		o = s.taboption('logging', form.Flag, 'watchdog', _('Watchdog Mode'), _('Auto-restart worker process on crash'));
		o.default = o.enabled;

		o = s.taboption('logging', form.Value, 'log_file', _('Log File Path'), _('Custom log file path (e.g. /var/log/rtsproxy.log). Leave empty for system log.'));
		o.placeholder = '/var/log/rtsproxy.log';

		o = s.taboption('logging', form.Value, 'log_lines', _('Log Max Lines'), _('Maximum log file lines before rotation (default: 10000)'));
		o.datatype = 'uinteger';
		o.placeholder = '10000';

		o = s.taboption('logging', form.ListValue, 'log_level', _('Log Level'), _('Set log verbosity level (default: info)'));
		o.value('error', _('Error'));
		o.value('warn', _('Warning'));
		o.value('info', _('Info'));
		o.value('debug', _('Debug'));
		o.default = 'info';

		s = m.section(form.GridSection, 'upstream_route', _('Upstream Routes'), _('Select an interface by destination CIDR. Longest prefix wins; unmatched destinations use the system routing table.'));
		s.addremove = true;
		s.anonymous = true;

		o = s.option(form.Value, 'cidr', _('Destination CIDR'));
		o.datatype = 'cidr4';
		o.placeholder = '192.168.2.0/24';
		o.rmempty = false;

		o = s.option(widgets.DeviceSelect, 'interface', _('Interface'));
		o.noaliases = true;
		o.rmempty = false;

		s = m.section(form.GridSection, 'rewrite', _('URL Rewrite Rules'));
		s.addremove = true;
		s.anonymous = true;

		o = s.option(form.ListValue, 'action', _('Action'));
		o.value('remove', _('Remove'));
		o.value('replace', _('Replace'));
		o.value('timeshift', _('Time Shift'));
		o.rmempty = false;

		o = s.option(form.Value, 'match', _('Match'));
		o.rmempty = false;

		o = s.option(form.Value, 'replacement', _('Replacement'));
		o.depends('action', 'replace');

		o = s.option(form.Value, 'shift_hours', _('Shift Hours'));
		o.datatype = 'integer';
		o.depends('action', 'timeshift');

		return m.render();
	}
});
