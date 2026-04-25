"use strict";
"require form";
"require view";
"require tools.widgets as widgets";

return view.extend({
	render: function () {
		var m, s, o;

		m = new form.Map('rtsproxy', _('RTSProxy'),
			_('High Performance RTSP Proxy with URL rewriting and NAT traversal.'));

		s = m.section(form.NamedSection, 'main', 'rtsproxy', _('General Settings'));


		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.rmempty = false;

		o = s.option(form.Value, 'port', _('Port'), _('Main listening port (default: 8554)'));
		o.datatype = 'port';
		o.placeholder = '8554';

		o = s.option(form.Flag, 'enable_nat', _('Enable NAT'), _('Enable NAT traversal (HTTP mode only)'));

		o = s.option(form.Value, 'rtp_buffer_size', _('RTP Buffer Size'), _('Number of packets in RTP buffer (default: 8192)'));
		o.datatype = 'uinteger';
		o.placeholder = '8192';

		o = s.option(form.Value, 'udp_packet_size', _('UDP Packet Size'), _('UDP packet size base (default: 1500)'));
		o.datatype = 'uinteger';
		o.placeholder = '1500';

		o = s.option(form.Value, 'auth_token', _('Auth Token'), _('Optional token for authentication'));
		o.password = true;

		o = s.option(widgets.DeviceSelect, 'http_interface', _('HTTP Upstream Interface'), _('Interface for HTTP proxy mode (IPTV)'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.option(widgets.DeviceSelect, 'mitm_interface', _('MITM Upstream Interface'), _('Interface for MITM transparent mode'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.option(widgets.DeviceSelect, 'listen_interface', _('Listen Interface'), _('Interface to listen on (Downstream)'));
		o.noaliases = true;
		o.rmempty = true;

		o = s.option(form.Value, 'json_path', _('JSON Config Path'), _('Path to URL rewrite rules (default: /etc/rtsproxy/config.json)'));
		o.placeholder = '/etc/rtsproxy/config.json';

		o = s.option(form.Flag, 'watchdog', _('Watchdog Mode'), _('Auto-restart worker process on crash'));
		o.default = o.enabled;

		return m.render();
	}
});
