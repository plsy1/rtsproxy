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
		s.tab('nat', _('NAT Settings'));
		s.tab('logging', _('Logging Settings'));

		o = s.taboption('basic', form.Flag, 'enabled', _('Enable'));
		o.rmempty = false;

		var port = uci.get('rtsproxy', 'main', 'port') || '8554';
		o = s.taboption('basic', form.DummyValue, '_webui', _('Management Dashboard'));
		o.rawhtml = true;
		o.default = '<a class="btn cbi-button cbi-button-apply" href="http://' + window.location.hostname + ':' + port + '/admin/" target="_blank" style="margin-top: 5px; display: inline-block;">' + _('Open WebUI') + '</a>';

		o = s.taboption('basic', form.Value, 'port', _('Port'), _('Main listening port (default: 8554)'));
		o.datatype = 'port';
		o.placeholder = '8554';

		o = s.taboption('nat', form.Flag, 'enable_nat', _('Enable NAT'), _('Enable NAT traversal'));

		o = s.taboption('nat', form.ListValue, 'nat_method', _('NAT Method'), _('Select NAT traversal method (default: stun)'));
		o.value('stun', _('STUN Mode'));
		o.value('zte', _('ZTE STB Hole Punching'));
		o.default = 'stun';
		o.depends('enable_nat', '1');

		o = s.taboption('basic', form.Value, 'rtp_buffer_size', _('RTP Buffer Size'), _('Number of packets in RTP buffer (default: 8192)'));
		o.datatype = 'uinteger';
		o.placeholder = '8192';

		o = s.taboption('basic', form.Value, 'udp_packet_size', _('UDP Packet Size'), _('UDP packet size base (default: 2048)'));
		o.datatype = 'uinteger';
		o.placeholder = '2048';

		o = s.taboption('basic', form.Value, 'auth_token', _('Auth Token'), _('Optional token for authentication'));
		o.password = true;

		o = s.taboption('basic', widgets.DeviceSelect, 'http_interface', _('HTTP Upstream Interface'), _('Interface for HTTP proxy mode (IPTV)'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.taboption('basic', widgets.DeviceSelect, 'mitm_interface', _('MITM Upstream Interface'), _('Interface for MITM transparent mode'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.taboption('basic', widgets.DeviceSelect, 'listen_interface', _('Listen Interface'), _('Interface to listen on (Downstream)'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.taboption('basic', form.Value, 'json_path', _('JSON Config Path'), _('Path to URL rewrite rules (default: /etc/rtsproxy/config.json)'));
		o.placeholder = '/etc/rtsproxy/config.json';

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

		return m.render();
	}
});
