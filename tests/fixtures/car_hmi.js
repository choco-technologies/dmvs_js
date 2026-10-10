        // --- 1. Zegar (Górny pasek) ---
        function updateClock() {
            const now = new Date();
            const timeStr = now.toLocaleTimeString('pl-PL', { hour: '2-digit', minute: '2-digit' });
            document.getElementById('clock').textContent = timeStr;
        }
        setInterval(updateClock, 1000);
        updateClock();

        // --- 2. Nawigacja między aplikacjami ---
        const navBtns = document.querySelectorAll('.nav-btn');
        navBtns.forEach(btn => {
            btn.addEventListener('click', () => {
                // Resetowanie stylów wszystkich przycisków
                navBtns.forEach(b => {
                    b.classList.remove('text-white', 'bg-slate-700/50', 'shadow-inner', 'active');
                    b.classList.add('text-slate-400');
                });
                // Ustawienie aktywnego przycisku
                btn.classList.add('text-white', 'bg-slate-700/50', 'shadow-inner', 'active');
                btn.classList.remove('text-slate-400');
                
                // Ukrycie wszystkich widoków
                document.querySelectorAll('.view').forEach(v => {
                    v.classList.remove('active');
                });
                
                // Pokazanie wybranego widoku
                const target = btn.getAttribute('data-target');
                document.getElementById('view-' + target).classList.add('active');
                
                // Zresetuj animację prędkościomierza, jeśli wchodzimy na ekran główny
                if(target === 'home') {
                    animateSpeedometer();
                }
            });
        });

        // --- 3. Animacja Prędkościomierza na Home ---
        const speedCircle = document.getElementById('speed-active');
        const speedText = document.getElementById('speed-text');
        
        function animateSpeedometer() {
            // Obwód dla r=68 to ok 427.2. 
            // Początkowo pasek jest całkowicie schowany (offset=427)
            speedCircle.style.transition = 'none';
            speedCircle.setAttribute('stroke-dashoffset', '427.2');
            speedText.innerText = '0';
            
            setTimeout(() => {
                // Obliczamy docelowy offset dla prędkości 68 km/h.
                // Widoczna część to 270 stopni (3/4 koła) = dł. 320.4
                // Proporcja 68 ze 160 (max) to ok. 42.5%
                // Długość kreski = 320.4 * 0.425 = 136
                // Docelowy offset = 427.2 - 136 = 291.2
                
                speedCircle.style.transition = 'stroke-dashoffset 1.5s cubic-bezier(0.4, 0, 0.2, 1)';
                speedCircle.setAttribute('stroke-dashoffset', '291.2');
                
                let currentSpeed = 0;
                const targetSpeed = 68;
                const interval = setInterval(() => {
                    currentSpeed += 2;
                    if(currentSpeed >= targetSpeed) {
                        currentSpeed = targetSpeed;
                        clearInterval(interval);
                    }
                    speedText.innerText = currentSpeed;
                }, 35);
            }, 100);
        }
        // Uruchom przy starcie
        animateSpeedometer();

        // --- 4. Odtwarzacz MP3 (Playlista i Sterowanie) ---
        const playlist = [
            { id: 0, title: "Neonowe Noce", artist: "Synthwave Rider", duration: "3:45", durationSec: 225, cover: "https://images.unsplash.com/photo-1614613535308-eb5fbd3d2c17?q=80&w=400&auto=format&fit=crop" },
            { id: 1, title: "Cyberpunk City", artist: "Neo Tokyo", duration: "4:12", durationSec: 252, cover: "https://images.unsplash.com/photo-1515630278258-407f66498911?q=80&w=400&auto=format&fit=crop" },
            { id: 2, title: "Chill Vibes", artist: "Lofi Beats", duration: "2:50", durationSec: 170, cover: "https://images.unsplash.com/photo-1514525253161-7a46d19cd819?q=80&w=400&auto=format&fit=crop" },
            { id: 3, title: "Midnight Drive", artist: "Nightcall", duration: "5:05", durationSec: 305, cover: "https://images.unsplash.com/photo-1557672172-298e090bd0f1?q=80&w=400&auto=format&fit=crop" },
            { id: 4, title: "Retro Wave", artist: "80s Synth", duration: "3:30", durationSec: 210, cover: "https://images.unsplash.com/photo-1550684848-fac1c5b4e853?q=80&w=400&auto=format&fit=crop" }
        ];

        let currentTrackId = 0;
        let isPlaying = true;
        let currentSecond = 84; // Symulacja trwającego odtwarzania (1:24)

        const uiMediaBg = document.getElementById('media-bg');
        const uiCover = document.getElementById('now-playing-cover');
        const uiTitle = document.getElementById('now-playing-title');
        const uiArtist = document.getElementById('now-playing-artist');
        const uiTimeCurrent = document.getElementById('time-current');
        const uiTimeTotal = document.getElementById('time-total');
        const uiProgress = document.getElementById('media-progress');
        const playlistContainer = document.getElementById('playlist-container');
        
        const playBtn = document.getElementById('play-btn');
        const playIcon = document.getElementById('play-icon');
        const prevBtn = document.getElementById('prev-btn');
        const nextBtn = document.getElementById('next-btn');

        function formatTime(sec) {
            const m = Math.floor(sec / 60);
            const s = Math.floor(sec % 60);
            return `${m}:${s.toString().padStart(2, '0')}`;
        }

        function renderPlaylist() {
            playlistContainer.innerHTML = '';
            playlist.forEach(track => {
                const isActive = track.id === currentTrackId;
                const div = document.createElement('div');
                div.className = `flex items-center gap-2 p-1.5 rounded-lg cursor-pointer transition-all duration-300 ${isActive ? 'bg-indigo-500/20 border border-indigo-500/30' : 'hover:bg-slate-800/50 border border-transparent'}`;
                div.onclick = () => loadTrack(track.id);
                
                div.innerHTML = `
                    <div class="w-8 h-8 rounded-md overflow-hidden shrink-0 border ${isActive ? 'border-indigo-400 shadow-[0_0_5px_rgba(99,102,241,0.5)]' : 'border-slate-700'} relative">
                        <img src="${track.cover}" class="w-full h-full object-cover">
                        ${isActive && isPlaying ? '<div class="absolute inset-0 bg-black/50 flex items-center justify-center"><div class="flex gap-[1.5px] h-[10px] items-end"><div class="w-[2px] bg-indigo-400 rounded-t animate-[bounce_0.8s_infinite_alternate]"></div><div class="w-[2px] bg-indigo-400 rounded-t animate-[bounce_1.1s_infinite_alternate]"></div><div class="w-[2px] bg-indigo-400 rounded-t animate-[bounce_0.9s_infinite_alternate]"></div></div></div>' : ''}
                    </div>
                    <div class="flex-1 min-w-0 flex flex-col justify-center">
                        <div class="text-[10px] font-bold ${isActive ? 'text-indigo-300' : 'text-slate-200'} truncate">${track.title}</div>
                        <div class="text-[8px] ${isActive ? 'text-indigo-400/80' : 'text-slate-400'} truncate">${track.artist}</div>
                    </div>
                    <div class="text-[8px] text-slate-500 font-medium px-1">${track.duration}</div>
                `;
                playlistContainer.appendChild(div);
            });
        }

        function loadTrack(id) {
            currentTrackId = id;
            currentSecond = 0;
            const track = playlist[currentTrackId];
            
            uiTitle.innerText = track.title;
            uiArtist.innerText = track.artist;
            uiTimeTotal.innerText = track.duration;
            uiCover.src = track.cover;
            uiMediaBg.style.backgroundImage = `url('${track.cover}')`;
            
            // Lekki efekt animacji zmiany utworu
            uiCover.classList.add('scale-110', 'opacity-70');
            setTimeout(() => uiCover.classList.remove('scale-110', 'opacity-70'), 300);

            if(!isPlaying) {
                isPlaying = true;
                playIcon.classList.replace('ph-play', 'ph-pause');
            }
            
            renderPlaylist();
            updateProgress();
        }

        function updateProgress() {
            const track = playlist[currentTrackId];
            if(isPlaying) {
                currentSecond++;
                if(currentSecond >= track.durationSec) {
                    playNext();
                    return;
                }
            }
            uiTimeCurrent.innerText = formatTime(currentSecond);
            const percent = (currentSecond / track.durationSec) * 100;
            uiProgress.style.width = percent + '%';
        }

        function playNext() {
            let nextId = currentTrackId + 1;
            if(nextId >= playlist.length) nextId = 0;
            loadTrack(nextId);
        }

        function playPrev() {
            let prevId = currentTrackId - 1;
            if(prevId < 0) prevId = playlist.length - 1;
            loadTrack(prevId);
        }

        playBtn.addEventListener('click', () => {
            isPlaying = !isPlaying;
            if(isPlaying) {
                playIcon.classList.replace('ph-play', 'ph-pause');
            } else {
                playIcon.classList.replace('ph-pause', 'ph-play');
            }
            renderPlaylist(); // Odświeża animację equalizera na playliście
        });
        
        nextBtn.addEventListener('click', playNext);
        prevBtn.addEventListener('click', playPrev);

        // Inicjalizacja paska postępu
        setInterval(updateProgress, 1000);
        
        // Pierwszy render Playlisty
        uiTimeCurrent.innerText = formatTime(currentSecond);
        renderPlaylist();

        // --- 5. Logika Klimatyzacji ---
        function changeTemp(side, change) {
            const el = document.getElementById('temp-' + side);
            let current = parseFloat(el.innerText);
            current += change;
            
            // Logika min/max
            if(current < 16.0) current = 16.0;
            if(current > 28.0) current = 28.0;
            
            el.innerHTML = current.toFixed(1) + '&deg;';
        }

        // Toggle przycisku A/C
        const acBtn = document.getElementById('ac-btn');
        acBtn.addEventListener('click', () => {
            acBtn.classList.toggle('bg-sky-500/20');
            acBtn.classList.toggle('text-sky-400');
            acBtn.classList.toggle('border-sky-500/50');
            acBtn.classList.toggle('shadow-[0_0_8px_rgba(14,165,233,0.3)]');
            
            acBtn.classList.toggle('bg-slate-800');
            acBtn.classList.toggle('text-slate-300');
            acBtn.classList.toggle('border-slate-700');
        });

    
