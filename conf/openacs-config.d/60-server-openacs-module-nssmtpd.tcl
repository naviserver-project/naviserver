#---------------------------------------------------------------------
# SMTPD proxy/server for NaviServer -- extra module "nssmtpd"
# ---------------------------------------------------------------------
# Outgoing mail for OpenACS
#
# To use this module:
#   1. Install the NaviServer nssmtpd module.
#   2. Set a nonempty $smtpdport.
#   3. Set the OpenACS package parameter
#        EmailDeliveryMode = nssmtpd
#      in acs-mail-lite. See:
#      https://openacs.org/xowiki/outgoing_email
#
ns_section ns/server/$server/modules {
    if {$smtpdport ne ""} {ns_param nssmtpd nssmtpd}
}
ns_section ns/server/$server/module/nssmtpd {
    #------------------------------------------------------------------
    # Basic binding and SMTP behaviour
    #------------------------------------------------------------------
    ns_param port        $smtpdport
    ns_param address     127.0.0.1            ;# local interface for SMTP server
    ns_param relay       $smtprelay           ;# upstream MTA or mail relay (e.g. localhost:25)
    ns_param spamd       localhost            ;# spamd/spamassassin daemon for filtering

    # SMTP processing callbacks (implemented in Tcl)
    ns_param initproc    smtpd::init
    ns_param rcptproc    smtpd::rcpt
    ns_param dataproc    smtpd::data
    ns_param errorproc   smtpd::error

    # Domain handling
    ns_param relaydomains "localhost"
    ns_param localdomains "localhost"

    #------------------------------------------------------------------
    # Logging and log rotation
    #------------------------------------------------------------------
    ns_param logging     on                   ;# default: off
    ns_param logfile     smtpsend.log
    ns_param logrollfmt  %Y-%m-%d             ;# appended to log filename on rotation

    # Optional rotation controls:
    #
    # ns_param logmaxbackup   100             ;# default: 10; max number of rotated logs
    # ns_param logroll        true            ;# default: true; auto-rotate logs
    # ns_param logrollonsignal true           ;# default: false; rotate on SIGHUP
    # ns_param logrollhour    0               ;# default: 0; hour of day for rotation

    #------------------------------------------------------------------
    # STARTTLS configuration (optional)
    #------------------------------------------------------------------
    # Enable STARTTLS and specify certificate chain files if needed.
    #
    # ns_param certificate "path/to/your/certificate-chain.pem"
    # ns_param cafile      ""
    # ns_param capath      ""
    #
    # Cipher suite selection (TLS 1.2 and below):
    # ns_param ciphers     "..."
}
