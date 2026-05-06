appending `?raw=1` works to get the plain text

```
curl -s "http://localhost/test.py?raw=1"
```

but this should also be possible by sending the request header `Accept: text/plain`

```
curl -s -H 'Accept: text/plain' http://localhost/test.py
```
