<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset='UTF-8'>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>NaviServer <%=[ns_info patchlevel]%> – Welcome</title>
  <link rel="stylesheet" href="doc/naviserver/man.css" type="text/css">
  <link rel="icon" type="image/svg+xml" href="favicon.svg">
  <style>
    body { font-family: Arial, sans-serif; background: #f9f9f9; margin: 0; padding: 0; }
    header { background: #004080; padding: 20px; color: #fff; }
    header a { color: #fff; text-decoration: none; font-size: 1.5em; }
    header span.tagline { font-size: 1em; margin-left: 10px; }
    .container { padding: 20px; }
    ul { list-style-type: disc; margin-left: 20px; }
    .config-details li { margin-bottom: 8px; }
    .security { background: #fff3cd; border: 1px solid #ffeeba; padding: 10px; margin-top: 20px; }
    a { color: #0066cc; }
    .btn-action {
      display: inline-block;
      padding: 2px 10px;
      margin-left: 15x;
      background-color: #007BFF; /* bright blue */
      color: #ffffff;
      text-decoration: none;
      border-radius: 4px;
      font-weight: bold; font-size: small;
      transition: background-color 0.3s ease, transform 0.2s ease;
    }
    .btn-action:hover, .btn-action:focus {
      background-color: #0056b3; /* A slightly darker blue for hover state */
      transform: translateY(-2px); /* lift effect */
    }
    div.container h1 {
      font-size: 2.5rem;          /* Large, prominent size */
      color: #333;                /* Dark grey for excellent readability */
      font-weight: 700;           /* Bold to draw attention */
      margin-top: 2.5rem;         /* Generous spacing above */
      margin-bottom: 1.5rem;      /* A little spacing below */
      line-height: 1.2;           /* Tighter line spacing for impact */
    }
    div.container h2 {
      font-size: 1.8rem;          /* Slightly smaller than h1 */
      color: #004080;             /* Using the same blue as the header for brand consistency */
      font-weight: 600;           /* Semi-bold for prominence */
      margin-top: 1.5rem;           /* Adequate spacing above */
      margin-bottom: 0.5rem;      /* Consistent spacing below */
      padding-bottom: 0.25rem;    /* Space for the underline */
      border-bottom: 2px solid #ccc;  /* Subtle underline to separate sections */
    }
    /* Modal overlay */
    .modal {
      display: none;
      position: fixed;
      z-index: 1000;
      left: 0;
      top: 0;
      width: 100%;
      height: 100%;
      overflow: auto;
      background-color: rgba(0,0,0,0.4);
    }
    /* Modal content box */
    .modal-content {
      background-color: #fefefe;
      margin: 10% auto;
      padding: 20px;
      border: 1px solid #888;
      width: 300px;
      border-radius: 5px;
    }
    .container .modal-content h2 {
      margin-top: 0.1rem;
    }
    /* Close button */
    .close {
      color: #aaa;
      float: right;
      font-size: 28px;
      font-weight: bold;
      cursor: pointer;
    }
    .close:hover,
    .close:focus {
      color: black;
    }
    /* Form element styles */
    label {
      display: block;
      margin-top: 10px;
      font-weight: bold;
    }
    input[type="password"] {
      width: 100%;
      padding: 8px;
      margin: 5px 0;
      box-sizing: border-box;
      border: 1px solid #ccc;
      border-radius: 4px;
    }
  </style>
</head>
<%
try {
    set modules [ns_ictl getmodules]
    set nsperm_warning ""
    set password_dialog ""
    if {"nsperm" in $modules} {
        try {ns_perm checkpass nsadmin x} on error {errorMsg} {} on ok {result} {
            set nsperm_warning [ns_trim -delimiter | [subst {
                |<div class="security">
                |  <strong>Security Alert:</strong>
                |  The 'nsperm' module is installed, but the default
                |  system administrator password for 'nsadmin' remains
                |  unchanged. Please update the password immediately.
                |
                |  <a href="#" id="changePwdBtn" class="btn-action">Change Password Now</a>
                |
                |  <div id="passwordModal" class="modal">
                |    <div class="modal-content">
                |      <span class="close">&times;</span>
                |      <h2>Change Password</h2>
                |      <form id="passwordForm">
                |        <!-- <label for="currentPwd">Current Password:</label>
                |        <input type="password" id="currentPwd" name="currentPwd" required> -->
                |
                |        <label for="newPwd">New Password:</label>
                |        <input type="password" id="newPwd" name="newPwd" required>
                |
                |        <label for="confirmPwd">Confirm New Password:</label>
                |        <input type="password" id="confirmPwd" name="confirmPwd" required>
                |
                |        <button type="submit" class="btn-action">Submit</button>
                |      </form>
                |    </div>
                |  </div>
                |</div>
            }]]
            set password_dialog [ns_trim -delimiter | {
                |<!-- JavaScript to Control the Modal Dialog -->
                |<script>
                |  // Get modal element and control elements
                |  var modal = document.getElementById('passwordModal');
                |  var btn = document.getElementById('changePwdBtn');
                |  var closeBtn = document.getElementsByClassName('close')[0];
                |
                |  // Open the modal when the button is clicked
                |  btn.onclick = function(e) {
                |    e.preventDefault();
                |    modal.style.display = 'block';
                |  }
                |
                |  // Close the modal when the close button is clicked
                |  closeBtn.onclick = function() {
                |    modal.style.display = 'none';
                |  }
                |
                |  // Close modal if user clicks outside the modal content
                |  window.onclick = function(event) {
                |    if (event.target == modal) {
                |      modal.style.display = 'none';
                |    }
                |  }
                |
                |  // Handle form submission with simple validation
                |  document.getElementById('passwordForm').addEventListener('submit', function(e) {
                |    e.preventDefault();
                |
                |    //var currentPwd = document.getElementById('currentPwd').value;
                |    var newPwd = document.getElementById('newPwd').value;
                |    var confirmPwd = document.getElementById('confirmPwd').value;
                |
                |    if(newPwd !== confirmPwd) {
                |      alert('New passwords do not match!');
                |      return;
                |    }
                |
                |    // Prepare the data in URL-encoded form
                |    var formData = new URLSearchParams();
                |    //formData.append('currentPwd', currentPwd);
                |    formData.append('newPwd', newPwd);
                |
                |    // Perform the AJAX request
                |    fetch('/change-nsperm-site-password', {
                |      method: 'POST',
                |      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                |      body: formData.toString()
                |    })
                |    .then(function(response) {
                |      if (!response.ok) {
                |        throw new Error('Network response was not ok: ' + response.statusText);
                |      }
                |      // console.log(response);
                |      return response.json(); // Expecting a JSON response
                |    })
                |   .then(function(data) {
                |      console.log(data);
                |      if (data.success) {
                |          alert('Password changed successfully!');
                |          location.reload();
                |      } else {
                |        alert('Error: ' + (data.error || 'Password change failed.'));
                |      }
                |      // Optionally close the modal dialog
                |      document.getElementById('passwordModal').style.display = 'none';
                |    })
                |    .catch(function(error) {
                |      alert('An error occurred: ' + error.message);
                |    });
                |  });
                |</script>
            }]
            ns_register_proc POST /change-nsperm-site-password {
                ns_permpasswd nsadmin x [ns_set get [ns_getform] newPwd]
                ns_return 200 text/json [ns_trim -delimiter | {{
                    |  "success": true,
                    |  "error": ""
                }}]
            }
        }
    }
    set nsstatsText {Explore the <a href="[dict get $nsstats href]">NaviServer Statistics Module</a>
        for real-time performance insights and logging analysis. [dict get $nsstats installHTML]
    }
    if { ![file exists [ns_server pagedir]/nsstats.tcl] } {
        dict set nsstats installHTML {
            <p class="install-info">
            Currently nsstats is not installed on your system.
            <a href="install-from-repository.tcl?package=nsstats" class="btn-action">Install Now</a>
            </p>
        }
        dict set nsstats href https://github.com/naviserver-project/nsstats
    } else {
        lappend modules nsstats
        dict set nsstats installHTML {}
        dict set nsstats href /nsstats.tcl
    }
    set includedModules {nscgi nscp nsdb nslog nsperm nsproxy nssock nsssl revproxy}
    set docPrefix [expr {[file exists [ns_server pagedir]/doc]
                         ? "/doc/"
                         : "https://naviserver.sourceforge.io/[ns_info version]"}]
    set modules [lmap m [lsort -unique $modules] {
        set href [expr {$m in $includedModules
                        ? "$docPrefix/$m/files/$m.html"
                        : "https://github.com/naviserver-project/$m"}]
        set html [subst {<a href="$href" [expr {$m in $includedModules ? "" : "class=external"}]>$m</a>}]
    }]
    set listenURLs {}
    set listenAddresses {}
    foreach di [ns_driver info] {
        set proto [dict get $di protocol]
        foreach addr [dict get $di address] {
            foreach port [dict get $di port] {
                if {$port != 0} {
                    lappend listenURLs [ns_joinurl [list host $addr port $port proto $proto]]
                    lappend listenAddresses $addr
                }
            }
        }
    }
    set listenAddresses [lsort -unique $listenAddresses]
    set maybePublic [expr {1 in [lmap addr $listenAddresses {expr {[ns_ip inany $addr] || [ns_ip public $addr]}}]
                           ? "(Note: Your server may be publicly accessible)"
                           : ""
                       }]
}

%>
<body>
  <header>
    <a href="/"><!-- <span class="logo">&nbsp;</span>--><strong>NaviServer</strong></a>
    <span class="tagline">Programmable Web Server</span>
    <style>
    .external::after {
      content: "↗";
      font-size: 0.7em;
      vertical-align: super;
      margin-left: 0px;
      color: #666;
    }
    </style>
</header>

  <div class="container">
    <h1>Welcome to NaviServer <%=[ns_info patchlevel]%></h1>
    <!-- <a href="https://sourceforge.net/projects/naviserver/">NaviServer</a> -->
    <p>
     Congratulations – your NaviServer instance is up and running on <%=[set . "$::tcl_platform(os) $::tcl_platform(osVersion)"]%>!
     This page confirms that your default installation is active.
    </p>

    <h2>Getting Started</h2>
    <p>
      To begin using NaviServer, you can:
    </p>
    <ul>
      <li>Replace this placeholder page with your custom content by configuring the appropriate directory.</li>
      <li>Review our <a href="doc/toc.html">Documentation</a> for complete setup instructions and feature details.</li>
       <li><%= [subst $nsstatsText]</li> %>
    </ul>

    <h2>Current Server Configuration</h2>
    <ul class="config-details">
      <li><strong>Configuration File:</strong> <code><%=[ns_info config]%></code></li>
      <li><strong>Listening:</strong> <%= [join [lsort $listenURLs] {, }] %> <%= $maybePublic %>
      </li>
      <li><strong>Loaded Modules:</strong> <%=[join $modules {, }]%></li>
    </ul>

  <!-- <li>The NaviServer <a href="examples/">Examples</a> include a few useful scripts and tricks.<p> -->

    <div class="security">
      <strong>Security Notice:</strong> For production environments, please replace or secure this default page to protect your server's details.
    </div>
    <%= $nsperm_warning %>
  </div>
  <%= $password_dialog %>
</body>
</html>



