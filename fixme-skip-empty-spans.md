input

```py
import urllib.parse
```

debug log

```
n=7
i=0 class=keyword start=0 end=6 len=6
i=1 class=variable start=7 end=13 len=6
i=2 class=constructor start=7 end=13 len=6
i=3 class=constant start=7 end=13 len=6
i=4 class=variable start=14 end=19 len=5
i=5 class=constructor start=14 end=19 len=5
i=6 class=constant start=14 end=19 len=5
```

output html

`<span class="keyword">import</span> <span class="variable">urllib</span><span class="constructor"></span><span class="constant"></span>.<span class="variable">parse</span><span class="constructor"></span><span class="constant"></span>`

expected: dont emit empty spans

```
i=2 class=constructor start=7 end=13 len=6
i=3 class=constant start=7 end=13 len=6

i=5 class=constructor start=14 end=19 len=5
i=6 class=constant start=14 end=19 len=5
```
