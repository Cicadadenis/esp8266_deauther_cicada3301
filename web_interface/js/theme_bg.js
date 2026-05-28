(function () {
    var c = document.getElementById("starsBg");
    if (!c) return;
    var ctx = c.getContext("2d");
    var w = 0, h = 0, dpr = 1;
    var stars = [];
    var N = 110;

    function resize() {
        dpr = Math.max(1, Math.min(2, window.devicePixelRatio || 1));
        w = window.innerWidth;
        h = window.innerHeight;
        c.width = Math.floor(w * dpr);
        c.height = Math.floor(h * dpr);
        c.style.width = w + "px";
        c.style.height = h + "px";
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        if (stars.length === 0) seed();
    }

    function seed() {
        stars = [];
        var i;
        for (i = 0; i < N; i++) {
            stars.push({
                x: Math.random() * w,
                y: Math.random() * h,
                r: 0.6 + Math.random() * 1.8,
                a: 0.22 + Math.random() * 0.6,
                s: 0.12 + Math.random() * 0.55
            });
        }
    }

    function step() {
        ctx.clearRect(0, 0, w, h);
        ctx.globalCompositeOperation = "lighter";
        var i;
        for (i = 0; i < stars.length; i++) {
            var s = stars[i];
            s.y += s.s;
            if (s.y > h + 6) {
                s.y = -6;
                s.x = Math.random() * w;
            }
            var g = ctx.createRadialGradient(s.x, s.y, 0, s.x, s.y, s.r * 8);
            g.addColorStop(0, "rgba(0,240,255," + (s.a * 0.95) + ")");
            g.addColorStop(0.4, "rgba(122,92,255," + (s.a * 0.55) + ")");
            g.addColorStop(1, "rgba(0,0,0,0)");
            ctx.fillStyle = g;
            ctx.beginPath();
            ctx.arc(s.x, s.y, s.r * 8, 0, Math.PI * 2);
            ctx.fill();
            ctx.fillStyle = "rgba(255,255,255," + (s.a * 0.4) + ")";
            ctx.beginPath();
            ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2);
            ctx.fill();
        }
        requestAnimationFrame(step);
    }

    window.addEventListener("resize", resize);
    resize();
    step();
})();
