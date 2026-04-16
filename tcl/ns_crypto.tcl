# -*- Tcl -*-
# Crypto support based on OpenSSL and nsf
#
# Author: Gustaf Neumann
# Date:   August 2016

#catch {package require nx}

if {[info commands ::nx::Class] eq ""} {
    ns_log warning "NSF is not installed. The commands ns_md, ns_hmac, ns_hotp and ns_totp are not available"
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
        fconfigure $F -translation binary
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
        if {[info exists :ctx]} {
            ::ns_crypto::md free ${:ctx}
        }
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
        if {[info exists :ctx]} {
            ::ns_crypto::hmac free ${:ctx}
        }
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
    counter
} {
    set hmac [::ns_crypto::hmac string -digest $digest $key $counter]
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
proc ns_uuid {} {
    return [ns_crypto::uuid -version v4]
}
# Tcl based version
#
# Uses the OpenSSL RAND_bytes function to generate a Version 4 UUID,
# which is meant for generating UUIDs from truly-random or
# pseudo-random numbers.
#
# proc ns_uuid {} {
#    set b [ns_crypto::randombytes 16]
#    set time_hi_and_version [string replace [string range $b 12 15] 0 0 4]
#    set clk_seq_hi_res      [string range $b 16 17]
#    set clk_seq_hi_res2     [format %2x [expr {("0x$clk_seq_hi_res" & 0x3f) | 0x80}]]
#    format %s-%s-%s-%s%s-%s \
#        [string range $b 0 7] \
#        [string range $b 8 11] \
#        $time_hi_and_version \
#        $clk_seq_hi_res2 \
#        [string range $b 18 19] \
#        [string range $b 20 31]
#}

# %  Package uuid
# 1.0.7
# % time {::uuid::uuid generate} 100000
# 366.14292850000004 microseconds per iteration

# Purely Tcl based variant
# % time {ns_uuid} 100000
# 5.559969000000001 microseconds per iteration

# C-based variant UUIDv4
# %  time {ns_crypto::uuid} 100000
# 0.5554475 microseconds per iteration

# C-based variant UUIDv7
# %  time {ns_crypto::uuid -version v7} 100000
# 0.56665875 microseconds per iteration


#                Time        Factor
# Tcllib 1.0.7   366,1429     1,00
# ns Tcl based     5,5600    65,85
# ns C based v4    0,5554   659,19
# ns C based v7    0,5667   646,14

#
# Tcl-level JWT helper built on top of:
#   - ns_json
#   - ns_base64urlencode / ns_base64urldecode
#   - ns_crypto::signature verify
#   - ns_crypto::key import
#

nx::Class create ::ns_crypto::JWT {

    #
    # Public methods
    #
    :public method encode {
        -alg:required
        {-key ""}
        {-jwk ""}
        {-secret ""}
        {-kid ""}
        {-typ "JWT"}
        {-cty ""}
        {-extraheader ""}
        -iss
        -sub
        -aud
        -exp
        -nbf
        -iat
        -jti
        {extrapayload ""}
    } {
        #
        # Create a JSON Web Token (JWT) in compact serialization format.
        #
        # The method builds a protected header and payload, encodes both
        # as base64url, and signs the resulting input using the specified
        # algorithm. The payload is constructed from standard JWT claims
        # provided as named parameters and optional additional claims in
        # triple form.
        #
        # @param alg Signature algorithm (e.g., EdDSA, ES256, ES256K, ES384,
        #        ES512, RS256, RS384, RS512, or "none"). When "none" is
        #        specified, no signature is added.
        # @param key Private key in PEM format (string or file) used for
        #        signing.
        # @param jwk JWK representation of the private key. Support for signing
        #        from JWK is reserved for future use and is not implemented yet.
        # @param secret Shared secret used for HS256, HS384, and HS512.
        # @param kid Optional key identifier to be included in the JWT
        #        header.
        # @param typ Optional type header (defaults to "JWT").
        # @param cty Optional content type header.
        # @param extraheader Additional header fields in triple form
        #        (name type value ...), merged into the protected header.
        # @param iss Issuer claim.
        # @param sub Subject claim.
        # @param aud Audience claim. May be a single value or a list of
        #        values. Multiple values are encoded as a JSON array.
        # @param exp Expiration time (numeric, seconds since epoch).
        # @param nbf Not-before time (numeric, seconds since epoch).
        # @param iat Issued-at time (numeric, seconds since epoch).
        # @param jti JWT ID claim.
        # @param extrapayload Additional payload fields in triple form
        #        (name type value ...), appended to the payload.
        #
        # @return A JWT string in compact form "header.payload.signature".
        #         When alg is "none", the signature part is empty.
        #

        set headerJson [:build_protected_header \
                            -alg $alg \
                            -kid $kid \
                            -typ $typ \
                            -cty $cty \
                            -extraheader $extraheader]
        set triples {}
        foreach field {iss sub aud exp nbf iat jti} {
            if {![info exists $field]} continue
            switch $field {
                exp -
                nbf -
                iat { lappend triples $field number [set $field] }
                aud {
                    if {[llength $aud] == 1} {
                        lappend triples aud string $aud
                    } else {
                        set array [lmap a $aud {list 1 string $a}]
                        lappend triples aud array [concat {*}$array]
                    }
                }
                default { lappend triples $field string [set $field] }
            }
        }
        lappend triples {*}$extrapayload

        set payloadJson [ns_json value -type object $triples]
        #ns_log notice payloadJson $payloadJson

        set headerB64  [ns_base64urlencode -- $headerJson]
        set payloadB64 [ns_base64urlencode -- $payloadJson]
        set signingInput "${headerB64}.${payloadB64}"

        if {$alg in {HS256 HS384 HS512}} {
            if {$secret eq ""} {
                error "missing shared secret; provide -secret"
            }
            set signature [:hmac_sign -alg $alg -secret $secret -data $signingInput]
        } elseif {$alg eq "none"} {
            # "-alg none" can be useful for decoding tests/debugging,
            return "${signingInput}."
        } else {
            set pem [:resolve_signing_key_pem \
                         -alg $alg \
                         -key $key \
                         -jwk $jwk]
            set signature [:sign -alg $alg -pem $pem -data $signingInput]
        }

        set sigB64 [ns_base64urlencode -binary -- $signature]

        return "${signingInput}.${sigB64}"
    }

    :public method decode {token} {
        #
        # Decode a JSON Web Token (JWT) without performing signature verification.
        #
        # The method splits the token into its three components, decodes the
        # protected header and payload from base64url, and parses them as JSON.
        # The signature part is returned as raw binary data.
        #
        # @param token JWT string in compact serialization format
        #        ("header.payload.signature").
        #
        # @return A Tcl dictionary with the following keys:
        #         header    — parsed JWT header as Tcl dictionary
        #         payload   — parsed JWT payload as Tcl dictionary
        #         signature — raw binary signature (empty for "alg=none")

        lassign [:split_token $token] headerB64 payloadB64 sigB64

        set headerJson  [ns_base64urldecode -- $headerB64]
        set payloadJson [ns_base64urldecode -- $payloadB64]

        set header  [ns_json parse $headerJson]
        set payload [ns_json parse $payloadJson]

        return [dict create \
                    header $header \
                    payload $payload \
                    signature [ns_base64urldecode -binary -- $sigB64]]
    }

    :public method verify {
        {-alg ""}
        {-key ""}
        {-jwk ""}
        {-jwks ""}
        {-secret ""}
        {-kid ""}
        {-requirekid:switch}
        {-verifyclaims:switch}
        {-aud ""}
        {-iss ""}
        {-sub ""}
        {-clockskew 0}
        {-now ""}
        token
    } {
        #
        # Verify a JSON Web Token (JWT) and optionally validate claims.
        #
        # The method decodes the token, resolves a verification key, and
        # verifies the signature according to the algorithm specified in
        # the JWT header. Optionally, registered claims can be validated.
        #
        # @param alg Expected signature algorithm. When specified, the
        #        value must match the "alg" field in the JWT header.
        # @param key Public key in PEM format (string or file) used for
        #        verification.
        # @param jwk JWK representation of the public key used for
        #        verification
        # @param jwks JWK set (list or dictionary of JWKs). When specified,
        #        the key is selected based on the "kid" value.
        # @param secret Shared secret used for HS256, HS384, and HS512.
        # @param kid Expected key identifier. When specified, it must match
        #        the "kid" value in the JWT header.
        # @param requirekid When true, require that the JWT header contains
        #        a "kid" field.
        # @param verifyclaims When true, validate registered claims such as
        #        expiration, not-before, issuer, subject, and audience.
        # @param aud Expected audience claim.
        # @param iss Expected issuer claim.
        # @param sub Expected subject claim.
        # @param clockskew Allowed clock skew in seconds when validating
        #        time-based claims.
        # @param now Reference time (seconds since epoch) used for claim
        #        validation. When not specified, the current time is used.
        # @param token JWT string in compact serialization format
        #        ("header.payload.signature").
        #
        # @return A Tcl dictionary containing:
        #         valid   — boolean indicating successful verification
        #         header  — parsed JWT header as Tcl dictionary
        #         payload — parsed JWT payload as Tcl dictionary
        #         kid     — resolved key identifier (may be empty)
        #         alg     — algorithm used for verification
        #

        lassign [:split_token $token] headerB64 payloadB64 sigB64

        set headerJson  [ns_base64urldecode -- $headerB64]
        set payloadJson [ns_base64urldecode -- $payloadB64]
        set signature   [ns_base64urldecode -binary -- $sigB64]

        set header  [ns_json parse $headerJson]
        set payload [ns_json parse $payloadJson]

        set tokenAlg [dict get $header alg]
        if {$alg ne "" && $alg ne $tokenAlg} {
            error "JWT algorithm mismatch: expected \"$alg\", got \"$tokenAlg\""
        }

        if {$tokenAlg eq "none"} {
            error "unsigned JWTs (alg=none) are not accepted by verify"
        }

        set signingInput "${headerB64}.${payloadB64}"
        set resolvedKid [expr {[dict exists $header kid] ? [dict get $header kid] : ""}]

        if {$requirekid && $resolvedKid eq ""} {
            error "JWT header does not contain required kid"
        }
        if {$kid ne "" && $resolvedKid ne $kid} {
            error "JWT kid mismatch: expected \"$kid\", got \"$resolvedKid\""
        }

        if {$tokenAlg in {HS256 HS384 HS512}} {
            if {$secret eq ""} {
                error "missing shared secret; provide -secret"
            }
            if {![:verify_hmac -alg $tokenAlg -secret $secret -data $signingInput -signature $signature]} {
                error "JWT signature verification failed"
            }

        } else {

            set verifyPem [:resolve_verification_key_pem \
                               -alg $tokenAlg \
                               -key $key \
                               -jwk $jwk \
                               -jwks $jwks \
                               -kid $resolvedKid]

            set verifySpec [:alg_to_verify_spec $tokenAlg]

            if {![:verify_signature \
                      -spec $verifySpec \
                      -pem $verifyPem \
                      -data $signingInput \
                      -signature $signature]} {
                error "JWT signature verification failed"
            }
        }

        if {$verifyclaims} {
            :verify_registered_claims \
                -payload $payload \
                -aud $aud \
                -iss $iss \
                -sub $sub \
                -clockskew $clockskew \
                -now $now
        }

        return [dict create \
                    valid 1 \
                    header $header \
                    payload $payload \
                    kid $resolvedKid \
                    alg $tokenAlg]
    }

    #
    # Protected helpers
    #
    :method split_token {token} {
        set parts [split $token .]
        if {[llength $parts] != 3} {
            error "invalid JWT: expected 3 dot-separated components"
        }
        return $parts
    }

    :method build_protected_header {
        {-alg:required}
        {-kid ""}
        {-typ "JWT"}
        {-cty ""}
        {-extraheader ""}
    } {
        set triples [list alg string $alg]
        if {$typ ne ""} {
            lappend triples typ string $typ
        }
        if {$cty ne ""} {
            lappend triples cty string $cty
        }
        if {$kid ne ""} {
            lappend triples kid string $kid
        }
        if {$extraheader ne ""} {
            foreach {k t v} $extraheader {
                lappend triples $k $t $v
            }
        }
        set json [ns_json value -type object $triples]
        return $json
    }

    :method resolve_signing_key_pem {
        {-alg ""}
        {-key ""}
        {-jwk ""}
    } {
        if {$key ne ""} {
            return $key
        }
        if {$jwk ne ""} {
            #
            # Later, private JWK import could be supported here.
            # For now, keep encode PEM/private-key oriented.
            #
            error "encode with -jwk is not implemented yet; provide -key PEM"
        }
        error "missing signing key; provide -key"
    }

    :method resolve_verification_key_pem {
        {-alg ""}
        {-key ""}
        {-jwk ""}
        {-jwks ""}
        {-kid ""}
    } {
        if {$key ne ""} {
            return $key
        }
        if {$jwk ne ""} {
            return [:public_key_from_jwk $jwk]
        }
        if {$jwks ne ""} {
            set jwk [:select_jwk_from_jwks -jwks $jwks -alg $alg -kid $kid]
            return [:public_key_from_jwk $jwk]
        }
        error "missing verification key; provide -key, -jwk, or -jwks"
    }

    :method select_jwk_from_jwks {
        {-jwks ""}
        {-alg ""}
        {-kid ""}
    } {
        if {![dict exists $jwks keys]} {
            error "invalid JWKS: missing \"keys\""
        }

        set matches {}
        foreach jwk [dict get $jwks keys] {
            if {$kid ne "" && [dict exists $jwk kid] && [dict get $jwk kid] ne $kid} {
                continue
            }
            if {$kid ne "" && ![dict exists $jwk kid]} {
                continue
            }
            if {[:jwk_supports_alg $jwk $alg]} {
                lappend matches $jwk
            }
        }

        if {[llength $matches] == 0} {
            error "no matching JWK found in JWKS"
        }
        if {[llength $matches] > 1} {
            error "JWKS lookup is ambiguous; multiple keys match"
        }
        return [lindex $matches 0]
    }

    :method jwk_supports_alg {jwk alg} {
        #
        # Conservative compatibility check.
        # If "alg" is present in the JWK, require exact match.
        # Otherwise infer from kty/crv.
        #
        if {[dict exists $jwk alg]} {
            return [string equal [dict get $jwk alg] $alg]
        }

        dict with jwk {
            switch -- $alg {
                ES256  {return [expr {$kty eq "EC"  && $crv eq "P-256"     }]}
                ES256K {return [expr {$kty eq "EC"  && $crv eq "secp256k1" }]}
                ES384  {return [expr {$kty eq "EC"  && $crv eq "P-384"     }]}
                ES512  {return [expr {$kty eq "EC"  && $crv eq "P-521"     }]}
                RS256 -
                RS384 -
                RS512  {return [expr {$kty eq "RSA"}]}
                EdDSA  {return [expr {$kty eq "OKP" && $crv in {"Ed25519" "Ed448"}}]}
                default {
                    return 0
                }
            }
        }
    }

    :method sign {
        {-alg ""}
        {-pem ""}
        {-data ""}
    } {
        switch -- $alg {
            ES256  {return [ns_crypto::signature sign -digest sha256 -encoding binary -pem $pem -- $data]}
            ES256K {return [ns_crypto::signature sign -digest sha256 -encoding binary -pem $pem -- $data]}
            ES384  {return [ns_crypto::signature sign -digest sha384 -encoding binary -pem $pem -- $data]}
            ES512  {return [ns_crypto::signature sign -digest sha512 -encoding binary -pem $pem -- $data]}
            RS256  {return [ns_crypto::signature sign -digest sha256 -encoding binary -pem $pem -- $data]}
            RS384  {return [ns_crypto::signature sign -digest sha384 -encoding binary -pem $pem -- $data]}
            RS512  {return [ns_crypto::signature sign -digest sha512 -encoding binary -pem $pem -- $data]}
            EdDSA  {return [ns_crypto::signature sign                -encoding binary -pem $pem -- $data]}
            default {
                error "unsupported JWT algorithm \"$alg\""
            }
        }
    }

    :method verify_signature {
        {-spec ""}
        {-pem ""}
        {-data ""}
        {-signature ""}
    } {
        #
        # Reuse the test helper mapping here.
        # E.g. spec might be:
        #   {digest sha256}
        #   {digest sha384}
        #   {digest sha512}
        #   {}
        #
        set cmd [list ns_crypto::signature verify]
        foreach {k v} $spec {
            lappend cmd -$k $v
        }
        lappend cmd -pem $pem -signature $signature -- $data
        #ns_log notice "DEBUG final command: $cmd"
        return [uplevel 1 $cmd]
    }

    :method verify_registered_claims {
        {-payload ""}
        {-aud ""}
        {-iss ""}
        {-sub ""}
        {-clockskew 0}
        {-now ""}
    } {
        if {$now eq ""} {
            set now [clock seconds]
        }

        if {[dict exists $payload exp]} {
            set exp [dict get $payload exp]
            if {$now > ($exp + $clockskew)} {
                error "JWT expired"
            }
        }

        if {[dict exists $payload nbf]} {
            set nbf [dict get $payload nbf]
            if {$now < ($nbf - $clockskew)} {
                error "JWT not valid yet"
            }
        }

        if {[dict exists $payload iat]} {
            set iat [dict get $payload iat]
            if {$iat > ($now + $clockskew)} {
                error "JWT issued-at time is in the future"
            }
        }

        if {$iss ne ""} {
            if {![dict exists $payload iss] || [dict get $payload iss] ne $iss} {
                error "JWT issuer mismatch"
            }
        }

        if {$sub ne ""} {
            if {![dict exists $payload sub] || [dict get $payload sub] ne $sub} {
                error "JWT subject mismatch"
            }
        }

        if {$aud ne ""} {
            if {![dict exists $payload aud]} {
                error "JWT audience missing"
            }
            set tokenAud [dict get $payload aud]
            if {[llength $tokenAud] > 1} {
                if {$aud ni $tokenAud} {
                    error "JWT audience mismatch"
                }
            } else {
                if {$tokenAud ne $aud} {
                    error "JWT audience mismatch"
                }
            }
        }
    }

    :method alg_to_verify_spec {alg} {
        switch -- $alg {
            ES256  {return {digest sha256}}
            ES256K {return {digest sha256}}
            ES384  {return {digest sha384}}
            ES512  {return {digest sha512}}
            RS256  {return {digest sha256}}
            RS384  {return {digest sha384}}
            RS512  {return {digest sha512}}
            EdDSA  {return {}}
            default {
                error "unsupported JWT algorithm \"$alg\""
            }
        }
    }

    :method public_key_from_jwk {jwk} {
        set kty [dict get $jwk kty]

        switch -- $kty {
            EC {
                set crv [dict get $jwk crv]
                set x   [ns_base64urldecode -binary -- [dict get $jwk x]]
                set y   [ns_base64urldecode -binary -- [dict get $jwk y]]

                set nsCurve [:jwk_ec_curve_to_ns_curve $crv]
                return [ns_crypto::key import \
                            -from public \
                            -name EC \
                            -params [list group $nsCurve x $x y $y] \
                            -format pem]
            }
            RSA {
                set n [ns_base64urldecode -binary -- [dict get $jwk n]]
                set e [ns_base64urldecode -binary -- [dict get $jwk e]]

                return [ns_crypto::key import \
                            -from public \
                            -name RSA \
                            -params [list n $n e $e] \
                            -format pem]
            }
            OKP {
                set crv [dict get $jwk crv]
                set x   [ns_base64urldecode -binary -- [dict get $jwk x]]

                return [ns_crypto::key import \
                            -from public \
                            -name OKP \
                            -params [list crv $crv x $x] \
                            -format pem]
            }
            default {
                error "unsupported JWK key type \"$kty\""
            }
        }
    }

    :method jwk_ec_curve_to_ns_curve {crv} {
        switch -- $crv {
            P-256      {return prime256v1}
            secp256k1  {return secp256k1}
            P-384      {return secp384r1}
            P-521      {return secp521r1}
            default {
                error "unsupported EC JWK curve \"$crv\""
            }
        }
    }

    # ---------------------------------------------
    # HMAC support
    # ---------------------------------------------
    :method alg_to_hmac_digest {alg} {
        switch -- $alg {
            HS256  { return sha256 }
            HS384  { return sha384 }
            HS512  { return sha512 }
            default {
                error "unsupported JWT HMAC algorithm \"$alg\""
            }
        }
    }

    :method hmac_sign {
        {-alg ""}
        {-secret ""}
        {-data ""}
    } {
        set digest [:alg_to_hmac_digest $alg]

        return [ns_crypto::hmac string \
                    -digest $digest \
                    -encoding binary \
                    -- $secret $data]
    }

    :method verify_hmac {
        {-alg ""}
        {-secret ""}
        {-data ""}
        {-signature ""}
    } {
        set expected [:hmac_sign \
                          -alg $alg \
                          -secret $secret \
                          -data $data]
        return [string equal $expected $signature]
    }

}
::ns_crypto::JWT create ns_jwt

#
# Local variables:
#    mode: tcl
#    tcl-indent-level: 4
#    indent-tabs-mode: nil
# End:
