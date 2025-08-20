/**
 * X-Server Proxy Gateway Example JavaScript File
 */

document.addEventListener('DOMContentLoaded', function() {
    console.log('X-Server Proxy Gateway Example Page Loaded');
    
    // Add page load time
    const loadTimeElement = document.createElement('div');
    loadTimeElement.className = 'load-time';
    loadTimeElement.style.textAlign = 'center';
    loadTimeElement.style.marginTop = '20px';
    loadTimeElement.style.color = '#7f8c8d';
    loadTimeElement.style.fontSize = '14px';
    
    const now = new Date();
    const timeString = now.toLocaleString('en-US');
    loadTimeElement.textContent = `Page Load Time: ${timeString}`;
    
    document.querySelector('.container').appendChild(loadTimeElement);
    
    // Add click events for links
    const links = document.querySelectorAll('a');
    links.forEach(link => {
        link.addEventListener('click', function(e) {
            console.log(`Clicked link: ${this.href}`);
        });
    });
});