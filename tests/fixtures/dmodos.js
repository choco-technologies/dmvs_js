        // Aktualizacja Zegara i Daty
        function updateClock() {
            const now = new Date();
            
            // Formatowanie czasu (np. 14:05)
            const hours = String(now.getHours()).padStart(2, '0');
            const minutes = String(now.getMinutes()).padStart(2, '0');
            const timeString = `${hours}:${minutes}`;
            
            // Tablice z polskimi nazwami dni i miesięcy
            const days = ['Niedziela', 'Poniedziałek', 'Wtorek', 'Środa', 'Czwartek', 'Piątek', 'Sobota'];
            const months = ['Sty', 'Lut', 'Mar', 'Kwi', 'Maj', 'Cze', 'Lip', 'Sie', 'Wrz', 'Paź', 'Lis', 'Gru'];
            
            const dayName = days[now.getDay()];
            const dayNum = now.getDate();
            const monthName = months[now.getMonth()];
            const dateString = `${dayName}, ${dayNum} ${monthName}`;
            
            // Aktualizacja w interfejsie
            document.getElementById('clock').innerText = timeString;
            document.getElementById('widget-time').innerText = timeString;
            document.getElementById('widget-date').innerText = dateString;
        }

        // Uruchom zegar
        updateClock();
        setInterval(updateClock, 10000); // Aktualizacja co 10 sekund jest wystarczająca

        // System zarządzania Oknami (Aplikacjami)
        const homeScreen = document.getElementById('home-screen');
        let currentApp = null;

        function openApp(appId) {
            const app = document.getElementById(appId);
            if(app) {
                app.classList.add('active');
                homeScreen.style.opacity = '0.3'; // Delikatne przyciemnienie tła pod spodem
                homeScreen.style.transform = 'scale(0.95)'; // Minimalne oddalenie ekranu domowego
                currentApp = app;
            }
        }

        function closeApp() {
            if(currentApp) {
                currentApp.classList.remove('active');
                homeScreen.style.opacity = '1';
                homeScreen.style.transform = 'scale(1)';
                currentApp = null;
            }
        }
    
