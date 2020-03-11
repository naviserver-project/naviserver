# -*- Tcl -*-
# Crypto support based on OpenSSL and nsf
#
# Author: Gustaf Neumann
# Date:   August 2016

#catch {package require nx}

if {[info commands ::nx::Class] eq ""} {
    ns_log warning "ns_md, ns_hmac, ns_hotp and ns_totp are not available"
    return
}

###########################################################################
# class ::ns_crypto::HashFunctions
#
#     Define common behavior for ns_crypto::* functionality
#
nx::Class create ::ns_crypto::HashFunctions {
    :property {digest sha256}
    :variable ctx

    :public method readfile {{-encoding hex} filename} {
        #
        # Read a file blockwise and call the incremental crypto
        # function on every block.
        #
        set F [open $filename]
        fconfigure $F -encoding binary -translation binary
        while (1) {
            set block [read $F 32768]
            :add $block
            if {[string length $block] < 32768} {
                break
            }
        }
        close $F
        #
        # Return the hash sum
        #
        return [:get -encoding $encoding]
    }
}

###########################################################################
# class ns_md
#
#     Provide an OO interface to the OpenSSL Message Digest
#     functionality.
#
nx::Class create ns_md -superclass ::ns_crypto::HashFunctions {

    :public object method string {{-digest sha256} {-encoding hex} message} {
        ::ns_crypto::md string -digest $digest -encoding $encoding $message
    }

    :public object method file {{-digest sha256} {-encoding hex} filename args} {
        set m [:new -digest $digest]
        set r ""
        foreach path [concat [list $filename] $args] {
            if {![file readable $path]} {
                $m destroy
                return -code error "file $path is not readable"
            }
            set r [$m readfile -encoding $encoding $path]
        }
        $m destroy
        return $r
    }

    :method init {} {
        set :ctx [::ns_crypto::md new ${:digest}]
    }
    :public method destroy {} {
        ::ns_crypto::md free ${:ctx}
        next
    }

    :public method add {message} {
        ::ns_crypto::md add ${:ctx} $message
    }
    :public method get {{-encoding hex}} {
        ::ns_crypto::md get -encoding $encoding ${:ctx}
    }
}


###########################################################################
# class ns_hmac
#
#     Provide an OO interface to the OpenSSL Hash Based Message
#     authentication Code (HMAC). In essence, this is a password
#     secured hash code.
#

nx::Class create ns_hmac -superclass ::ns_crypto::HashFunctions {
    :property key:required

    :public object method string {{-digest sha256} {-encoding hex} key message} {
        ::ns_crypto::hmac string -digest $digest -encoding $encoding $key $message
    }

    :public object method file {{-digest sha256} {-encoding hex} key filename args} {
        set m [:new -digest $digest -key $key]
        set r ""
        foreach path [concat [list $filename] $args] {
            if {![file readable $path]} {
                $m destroy
                return -code error "file $path is not readable"
            }
            set r [$m readfile -encoding $encoding $path]
        }
        $m destroy
        return $r
    }

    :method init {} {
        set :ctx [::ns_crypto::hmac new ${:digest} ${:key}]
    }
    :public method destroy {} {
        ::ns_crypto::hmac free ${:ctx}
        next
    }

    :public method add {message} {
        ::ns_crypto::hmac add ${:ctx} $message
    }
    :public method get {{-encoding hex}} {
        ::ns_crypto::hmac get -encoding $encoding ${:ctx}
    }
}

###########################################################################
# HOTP: Implementation of One-Time Passwords as described in RFC 4226
# based on a HMAC function.  RFC 4226 defines HOTP as:
#
#     HOTP(K,C) = Truncate(HMAC-SHA-1(K,C))
#
#    K: key
#    C: counter (moving factor for one time passwd)
#
# The function "ns_hotp" receives as input the digest algorithm, the
# number of digits of the resulting password, a key and data used for
# the HMAC (C in above formula).

nsf::proc ns_hotp {
    {-digest sha256}
    {-digits:integer 6}
    {-key ""}
    data
} {
    set hmac [::ns_crypto::hmac string -digest $digest $key $data]
    return [::ns_crypto::hotp_truncate -digits $digits $hmac]
}

#
# The RFC 4226 truncate function returns from a hexadecimal input a
# decimal number with $digits digits.
#
nsf::proc ::ns_crypto::hotp_truncate {
    -digits:integer
    input
} {
    #
    # offset is value of last byte
    #
    set offset  [expr {("0x[string range $input end-1 end]" & 0x0f) * 2}]

    #
    # Get the value of 4 bytes starting from offset, mask most significant bit
    #
    set dbc1 [expr {"0x[string range $input $offset $offset+7]" & 0x7fffffff}]
    #
    # DBC1 (stands for "dynamic binary code" in RFC 4226) is assumed to
    # be equal to DBC2. The same is done in the reference implementation
    # RFC 4226.
    #
    # Finally return last $digits digits of $dbc1
    return [string range $dbc1 end-[incr digits -1] end]
}



###########################################################################
# TOTP: Implementation of a Time-Based One-Time Password Algorithm as
# specified in RFC 6238 based on HOTP. TOTP is defined as
#
#     TOTP = HOTP(K, T), where T is an integer
#
#     K: key
#     T: time slice (moving factor for one time passwd)
#
# The RFC examples use 8 digits for output, the moving factor is there
# a 64 bit value.  Of course, in general the value can be different,
# but these are used here to obtain the same results as in test
# vectors in the RFC.

nsf::proc ns_totp {
    {-digest sha256}
    {-digits:integer 8}
    {-interval:integer 30}
    {-user_id:integer 0}
    {-key}
    {-time}
} {
    #
    # If no key is provided, get configured secret and personalize
    # this for the given user_id.
    #
    if {![info exists key]} {
        set secret [ns_config "ns/server/[ns_info server]" serversecret ""];
        set key [::ns_crypto::md string -digest sha224 -encoding binary $secret-$user_id]
    }
    #
    # If no time is provided, take the current time
    #
    if {![info exists time]} {set time [clock seconds]}
    #
    # Call the HTOP implementation for the time slice (return the same
    # value for interval seconds)
    #
    set totp [ns_hotp \
                  -digest $digest \
                  -digits $digits \
                  -key $key \
                  [binary format W [expr {$time / $interval}]]]
    return $totp
}

#
# ns_uuid: Generate a Version 4 UUID according to RFC 4122
#
# Uses the OpenSSL RAND_bytes function to generate a Version 4 UUID,
# which is meant for generating UUIDs from truly-random or
# pseudo-random numbers.
#
#   The algorithm is as follows:
#
#   o  Set the two most significant bits (bits 6 and 7) of the
#      clock_seq_hi_and_reserved to zero and one, respectively.
#
#   o  Set the four most significant bits (bits 12 through 15) of the
#      time_hi_and_version field to the 4-bit version number from
#      Section 4.1.3.
#
#   o  Set all the other bits to randomly (or pseudo-randomly) chosen values.
#
proc ns_uuid {} {
    set b [ns_crypto::randombytes 16]
    set time_hi_and_version [string replace [string range $b 12 15] 0 0 4]
    set clk_seq_hi_res      [string range $b 16 17]
    set clk_seq_hi_res2     [format %2x [expr {("0x$clk_seq_hi_res" & 0x3f) | 0x80}]]
    format %s-%s-%s-%s%s-%s \
        [string range $b 0 7] \
        [string range $b 8 11] \
        $time_hi_and_version \
        $clk_seq_hi_res2 \
        [string range $b 18 19] \
        [string range $b 20 31]
}
# % package require uuid
# 1.0.5
# % time {::uuid::uuid generate} 10000
# 366.14292850000004 microseconds per iteration
# % time {ns_uuid} 10000
# 5.559969000000001 microseconds per iteration

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
