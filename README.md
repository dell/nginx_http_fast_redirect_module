# Nginx HTTP Fast Redirect Module

The Nginx HTTP Fast Redirect module provides simple, highly-performant management of 3xx redirects in Nginx.
A 1-to-1 mapping of source paths to destinations is supplied by a CSV file, which is loaded into an in-memory hashmap for
optimal performance.

Additional CSV fields allow the definition of start and end dates during which the redirect should be active, Time to Live (TTL),
and the status code to use for the redirect response (301, 302, etc).

## Table of Contents

-   [Installation](#installation)
-   [Directives](#directives)
-   [CSV File Format](#csv-file-format)
-   [Module Directives](#module-directives)
-   [Configuration Examples](#configuration-examples)

## Installation

To compile the Nginx HTTP Fast Redirect module as a dynamic module:

1. Clone this repo
2. Download the Nginx source code from [the official Nginx website](https://nginx.org/en/download.html).
3. Extract the source code archive.
4. Navigate to the source code directory.
5. Run ./configure with the path to this repo

```
./configure --with-compat --add-dynamic-module=../path/to/nginx_http_fast_redirect_module
```

6. Run `make modules`

The dynamic module should then be built at `./objs/ngx_http_fast_redirect_module.so`

See [Compiling Third-Party Dynamic Modules for NGINX and NGINX Plus](https://www.nginx.com/blog/compiling-dynamic-modules-nginx-plus/)
for more information.

## Module Directives

### fast_redirect_store

**Syntax**: fast_redirect_store _name_ file=csv_file_path;

**Context**: http, server

**Description**: Defines a redirect store and specifies the path to the CSV file containing the redirection rules.

***

### fast_redirect

**Syntax**: fast_redirect _name_

**Context**: location

**Description**: Specifies the redirect store to be used for the current location. The `name` argument should match the name specified in the `fast_redirect_store` directive.

***

### fast_redirect_time_travel_cookie

**Syntax**: fast_redirect_time_travel_cookie _cookie_name_;

**Context**: http, server

**Description**: Sets the name of the cookie used for time travel functionality. If this directive is not specified, time travel functionality will be disabled.

This is useful for testing. A unix timestamp, provided as the value of a cookie instructs the module to act _as if_ it were a particular time.


## Configuration Example

Example Nginx configuration:

```nginx
http {
  fast_redirect_store myredirectstore file=/path/to/redirects.csv;

  server {
    listen 80;
    server_name example.com;

    location / {
      root   html;
      index  index.html index.htm;
      fast_redirect myredirectstore;
    }
  }
}
```

Note that because the Fast Redirect module operates in the rewrite phase it can co-exist with content handlers such as
`root` or `proxy_pass`. This is useful for conditional logic, such as "redirect if a redirect exists for the current
path, otherwise proxy to upstream." If a redirect does not exist for the requested path, the module takes no action, allowing
the content handler to complete the request.


## CSV File Format

The CSV file used by the Fast Redirect module should follow a specific format. Each line represents a redirection rule and consists of the following fields:

1. Source URL: The original URL to be redirected from.
2. Destination URL: The target URL to redirect to.
3. HTTP status code: The HTTP status code to use for the redirection.
4. Start time (optional): The start time for the redirection rule in UNIX timestamp format.
5. End time (optional): The end time for the redirection rule in UNIX timestamp format.
6. Max age (optional): The maximum time the redirection should be cached by clients in seconds.

The fields in each line should be separated by commas.

Example CSV file:
```
source, destination, max-age, http status, start time, end time
/source1,/destination1,301
/source2,/destination2,302,1627339800,1627343400,3600
/source3,/destination3,303,1627339800,,3600
```

Note the first line is expected to be a header, which will be skipped when processing.

In the above example:

-   The first line redirects `/source1` to `/destination1` using a 301 status code.
-   The second line redirects `/source2` to `/destination2` using a 302 status code. The redirection is active from 1627339800 to 1627343400 (UNIX timestamps) and has a maximum age of 3600 seconds.
-   The third line redirects `/source3` to `/destination3` using a 303 status code. The redirection is active from 1627339800 (UNIX timestamp) indefinitely and has a maximum age of 3600 seconds.
